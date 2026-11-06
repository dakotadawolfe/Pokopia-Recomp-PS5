#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rebuild RADV's PS5 allocator object in a private archive with a guest-safe VA arena.

Inputs are the raw driver and compile database from GRAPHICS_PORT.md's pinned
public source recipe. The original archive, source tree, SDK and build tree are
read-only. Only this object's source and compiler outputs are regenerated.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

RAW_SHA = "7bd32507c7dca420b69356a30f97940758035bdbddce2f99bec2b69010dd1ad6"
SOURCE_SHA = "8112c2b41a600043d3096a0198c4e3c2d7dcf6b56e8f738f6ce8eb537943725c"
MEMBER = "winsys_ps5_radv_ps5_platform.c.o"
RETIRE_MEMBER = "winsys_ps5_radv_ps5_bo.c.o"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_archive", type=Path)
    parser.add_argument("compile_database", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expected-raw-sha256", default=RAW_SHA)
    parser.add_argument("--retire-buffers", action="store_true", help="Backport fenced backing-memory retirement to the pinned driver")
    args = parser.parse_args()
    raw = args.raw_archive.resolve(strict=True)
    database = args.compile_database.resolve(strict=True)
    output = args.output.resolve()
    raw_hash = digest(raw)
    if raw_hash != args.expected_raw_sha256:
        raise ValueError("Raw driver differs from the explicit expected SHA-256")
    records = [entry for entry in json.loads(database.read_text()) if entry["file"].endswith("/radv_ps5_platform.c")]
    if len(records) != 1:
        raise ValueError("Expected exactly one PS5 platform compile command")
    record = records[0]
    build = Path(record["directory"]).resolve(strict=True)
    source = (build / record["file"]).resolve(strict=True)
    if digest(source) != SOURCE_SHA:
        raise ValueError("Platform source is not the reviewed public Mesa/Eglantine adaptation")
    if output == build or output == source.parent or output == raw.parent:
        raise ValueError("Use an independent output directory")
    marker = output / ".pokopia-radv-partition"
    if output.exists() and (not marker.is_file() or marker.read_text() != raw_hash):
        raise ValueError("Refusing to overwrite an unowned partition build directory")
    output.mkdir(parents=True, exist_ok=True)
    marker.write_text(raw_hash)
    original = source.read_text()
    patches = [
        ("/* The device-memory region: 256 GiB at 0x4000000000, handed out in 2 MiB",
         "/* The device-memory region: 128 GiB at 0x4000000000, handed out in 2 MiB"),
        ("#define RADV_PS5_REGION_BYTES (UINT64_C(256) << 30)",
         "#define RADV_PS5_REGION_BYTES (UINT64_C(128) << 30)"),
        ("                                 : va < RADV_PS5_GPU_ADDRESS_LIMIT && bytes <= RADV_PS5_GPU_ADDRESS_LIMIT - va);",
         "                                 : granules != 0 && address == hint && va >= RADV_PS5_REGION_BASE &&\n"
         "                                      va < RADV_PS5_REGION_BASE + RADV_PS5_REGION_BYTES &&\n"
         "                                      bytes <= RADV_PS5_REGION_BASE + RADV_PS5_REGION_BYTES - va);"),
    ]
    patched = original
    for before, after in patches:
        if patched.count(before) != 1:
            raise ValueError("Pinned allocator source anchor changed")
        patched = patched.replace(before, after)
    patched_source = output / "radv_ps5_platform.c"
    patched_source.write_text(patched)
    command = record.get("arguments") or shlex.split(record["command"])
    if Path(command[0]).name == "ccache":
        command = command[1:]
    compiler = Path(command[0]).resolve(strict=True)
    if compiler.name != "prospero-clang":
        raise ValueError("Expected the pinned SDK's C compiler wrapper")
    # Preserve every semantic option and original relative include path while
    # moving all compiler writes to this owned directory.
    filtered = [str(compiler)]
    skip = False
    for argument in command[1:]:
        if skip:
            skip = False
        elif argument in ("-o", "-c", "-MF", "-MQ", "-MT"):
            skip = True
        elif argument not in ("-MD", "-MMD", "-MP"):
            filtered.append(argument)
    obj = output / MEMBER
    filtered += ["-I" + str(source.parent), "-c", str(patched_source), "-o", str(obj)]
    environment = dict(os.environ, PS5_PAYLOAD_SDK=str(compiler.parent.parent))
    subprocess.run(filtered, cwd=build, env=environment, check=True)
    target = output / "libvulkan_radeon.ps5.a"
    shutil.copyfile(raw, target)
    members = subprocess.check_output(["ar", "t", str(raw)], text=True).splitlines()
    if members.count(MEMBER) != 1:
        raise ValueError("Platform object is not unique in the input archive")
    subprocess.run(["ar", "rD", str(target), str(obj)], check=True)
    retirement = None
    if args.retire_buffers:
        retirement = build_retirement(database, output, target, raw)
    subprocess.run(["ranlib", "-D", str(target)], check=True)
    if subprocess.check_output(["ar", "t", str(target)], text=True).splitlines() != members:
        raise ValueError("Archive member list changed while replacing the allocator")
    if digest(raw) != raw_hash or digest(source) != SOURCE_SHA:
        raise ValueError("An original input changed during the independent build")
    manifest = {
        "patch": "pokopia-guest-safe-radv-arena-v1", "arena_base": "0x4000000000", "arena_limit": "0x6000000000",
        "reject_outside_arena_fallback": True, "source_sha256": SOURCE_SHA,
        "patched_source_sha256": digest(patched_source), "input_archive_sha256": raw_hash,
        "output_archive_sha256": digest(target), "replacement_object_sha256": digest(obj),
        "compile_database_sha256": digest(database), "compile_command": filtered,
        "public_mesa_revision": "7b59ef27c1b09b9671bc4153c41940c3155c3af2",
        "public_patch_revision": "44e3f3e88fd6bd634102525a704bb5956589d507",
        "console_verified": False,
        "buffer_retirement": retirement,
    }
    (output / "partition-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Private partitioned driver: {target}\nsha256 {manifest['output_archive_sha256']}")


def build_retirement(database, output, target, raw):
    records = [entry for entry in json.loads(database.read_text()) if entry["file"].endswith("/radv_ps5_bo.c")]
    if len(records) != 1:
        raise ValueError("Expected exactly one PS5 buffer compile command")
    record = records[0]
    build = Path(record["directory"]).resolve(strict=True)
    source = (build / record["file"]).resolve(strict=True)
    source_hash = digest(source)
    expected = "1760f1bdfbf1cb26bbcd6986e40e4a9b3540d67c78a2ea9393f6824b0a3a9362"
    if source_hash != expected:
        raise ValueError("Buffer source differs from the reviewed Mesa pin")
    patch = Path(__file__).resolve().parent / "patches/radv-buffer-retirement.patch"
    patched = output / "radv_ps5_bo.c"
    shutil.copyfile(source, patched)
    subprocess.run(["patch", "--batch", "--fuzz=0", "-p6", "-i", str(patch)], cwd=output, check=True)
    command = record.get("arguments") or shlex.split(record["command"])
    if Path(command[0]).name == "ccache":
        command = command[1:]
    compiler = Path(command[0]).resolve(strict=True)
    if compiler.name != "prospero-clang":
        raise ValueError("Expected the pinned SDK compiler")
    filtered = [str(compiler)]
    skip = False
    for argument in command[1:]:
        if skip:
            skip = False
        elif argument in ("-o", "-c", "-MF", "-MQ", "-MT"):
            skip = True
        elif argument not in ("-MD", "-MMD", "-MP"):
            filtered.append(argument)
    obj = output / RETIRE_MEMBER
    filtered += ["-I" + str(source.parent), "-c", str(patched), "-o", str(obj)]
    subprocess.run(filtered, cwd=build, env=dict(os.environ, PS5_PAYLOAD_SDK=str(compiler.parent.parent)), check=True)
    members = subprocess.check_output(["ar", "t", str(raw)], text=True).splitlines()
    if members.count(RETIRE_MEMBER) != 1:
        raise ValueError("Buffer object is not unique in the input archive")
    subprocess.run(["ar", "rD", str(target), str(obj)], check=True)
    if digest(source) != source_hash:
        raise ValueError("Original buffer source changed")
    return {"upstream_commit": "f5610fe27090acd2509e5ce807140676f0b04473",
            "source_sha256": source_hash, "patch_sha256": digest(patch),
            "patched_source_sha256": digest(patched), "object_sha256": digest(obj),
            "compile_command": filtered, "console_verified": False}


if __name__ == "__main__":
    main()
