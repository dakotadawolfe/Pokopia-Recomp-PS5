#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the graphics probe's isolated RADV from its pinned raw archive.

Adapted from dakotadawolfe/EglantinePS5 tools/isolate-radv.py at
44e3f3e88fd6bd634102525a704bb5956589d507. Only implementation symbols are
renamed. The raw archive, public ICD entrypoints, and C++ runtime identities
remain unchanged. See GRAPHICS_PORT.md for the public source build inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

RAW_SHA256 = "7bd32507c7dca420b69356a30f97940758035bdbddce2f99bec2b69010dd1ad6"
ISOLATED_SHA256 = "ce6141d9c0c01afd8b20eee3ac8f031a3054f91dd8e4b2e2ae5ba40fbf100e4e"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def symbols(path, defined=False):
    command = ["nm", "-g", "--format=posix"]
    if defined:
        command.append("--defined-only")
    result = subprocess.run([*command, str(path)], check=True, capture_output=True, text=True)
    return {parts[0] for line in result.stdout.splitlines()
            if len(parts := line.split()) >= 2 and len(parts[1]) == 1}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expected-raw-sha256", default=RAW_SHA256)
    output_check = parser.add_mutually_exclusive_group()
    output_check.add_argument("--expected-isolated-sha256", default=ISOLATED_SHA256)
    output_check.add_argument("--record-rebuilt-output", action="store_true",
                              help="Explicitly record a fresh source build's output hash; does not qualify it on PS5")
    args = parser.parse_args()
    raw_sha = args.expected_raw_sha256
    if not re.fullmatch(r"[0-9a-f]{64}", raw_sha):
        raise ValueError("Expected raw hash must be a lowercase SHA-256")
    expected_output = None if args.record_rebuilt_output else args.expected_isolated_sha256
    if expected_output is not None and not re.fullmatch(r"[0-9a-f]{64}", expected_output):
        raise ValueError("Expected output hash must be a lowercase SHA-256")
    source = args.raw_archive.resolve(strict=True)
    output = args.output.resolve()
    target = output / "libvulkan_radeon.ps5.a"
    if source == target:
        raise ValueError("The isolated output must not overwrite its raw input")
    if digest(source) != raw_sha:
        raise ValueError("Raw RADV hash differs from the reviewed build; see the source rebuild notes before changing pins")
    marker = output / ".pokopia-radv-isolation"
    expected = {marker.name, target.name, "symbols.map", "manifest.json"}
    if output.exists():
        if not marker.is_file() or marker.read_text() != raw_sha:
            raise ValueError(f"Refusing to replace an unowned isolation directory: {output}")
        if any(path.name not in expected or not path.is_file() for path in output.iterdir()):
            raise ValueError(f"Unexpected contents in the isolation directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    marker.write_text(raw_sha)
    defined = sorted(symbols(source, True))
    demangled = subprocess.run(["c++filt"], input="\n".join(defined) + "\n",
                              check=True, capture_output=True, text=True).stdout.splitlines()
    if len(defined) != len(demangled) or "radv_GetInstanceProcAddr" not in defined:
        raise ValueError("Unexpected symbol table in the pinned raw archive")
    public = {name for name in defined if re.match(r"vk[A-Z]|vk_icd", name)} | {"radv_GetInstanceProcAddr"}
    private = []
    for name, readable in zip(defined, demangled):
        shared_cpp = ((re.match(r"^(typeinfo (for|name for) std::|vtable for std::)", readable) and "<" not in readable)
                      or readable.startswith(("operator new", "operator delete"))
                      or name.startswith(("__cxa_", "__gxx_", "_Unwind_")))
        if name not in public and not shared_cpp:
            private.append(name)
    mapping = output / "symbols.map"
    # Preserve the established prefix to reproduce the pinned binary exactly.
    mapping.write_text("".join(f"{name} eden_radv_private_{name}\n" for name in private))
    subprocess.run(["objcopy", "--redefine-syms=" + str(mapping), str(source), str(target)], check=True)
    subprocess.run(["ranlib", str(target)], check=True)
    if digest(source) != raw_sha:
        raise ValueError("The raw archive changed during isolation")
    actual = digest(target)
    if expected_output is not None and actual != expected_output:
        raise ValueError(f"Isolated archive differs from the reviewed output: {actual}; check GNU binutils versions")
    if set(private).intersection(symbols(target)) or not public <= symbols(target, True):
        raise ValueError("Namespace isolation did not preserve the expected symbol boundary")
    manifest = {
        "source": source.name, "source_sha256": raw_sha, "isolated_sha256": actual,
        "renamed_symbols": len(private), "public_symbols": sorted(public),
        "source_patches": {"url": "https://github.com/dakotadawolfe/EglantinePS5.git",
                           "revision": "44e3f3e88fd6bd634102525a704bb5956589d507"},
        "note": "Namespace/hash verification only; console validation is separate.",
        "recorded_source_rebuild": args.record_rebuilt_output,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified isolated RADV: {target}\nsha256 {actual}\nrenamed symbols {len(private)}")


if __name__ == "__main__":
    main()
