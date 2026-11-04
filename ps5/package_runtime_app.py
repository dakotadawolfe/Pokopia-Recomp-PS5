#!/usr/bin/env python3
"""Package the original runtime fixtures as a native ShadowMountPlus test app."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TITLE = "PPSA98731"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_fself(path):
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != bytes.fromhex("4f153d1d"):
        raise ValueError("Missing native FSELF header")
    elf = 32 + struct.unpack_from("<H", data, 24)[0] * 32
    if data[elf:elf + 6] != b"\x7fELF\x02\x01":
        raise ValueError("Missing embedded ELF64 header")
    table = elf + struct.unpack_from("<Q", data, elf + 32)[0]
    stride, count = struct.unpack_from("<HH", data, elf + 54)
    if stride != 56 or not count or table < elf + 64 or table + stride * count > len(data):
        raise ValueError("Invalid program header table")
    loads = 0
    for offset in range(table, table + stride * count, stride):
        kind, flags, file_offset, address, _, size, memory, alignment = struct.unpack_from("<II6Q", data, offset)
        if kind == 1 and flags:
            if alignment != 16384 or file_offset % 16384 or address % 16384 or size > memory:
                raise ValueError("Mapped native segment is not aligned to 16 KiB")
            loads += 1
    if not loads:
        raise ValueError("No mapped native segments")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("template", type=Path)
    parser.add_argument("tool", type=Path)
    args = parser.parse_args()
    app = args.build / "package" / TITLE
    copies = {"sce_sys/param.json": ROOT / "ps5/app/param.json",
              "sce_module/libc.prx": args.template / "runtime/libc.prx",
              "licenses/runtime-toolchain-GPL-3.0.txt": args.template / "LICENSE"}
    for name in ("icon0.png", "pic0.dds", "pic1.dds", "snd0.at9"):
        copies["sce_sys/" + name] = args.template / "sce_sys" / name
    for name in ("fixture_aarch64.elf", "fixture_nosections.elf", "fixture_relr.elf"):
        copies["tests/data/" + name] = ROOT / "runtime/tests/data" / name
    expected = set(copies) | {"eboot.bin", ".pokopia-runtime-test"}
    marker = app / ".pokopia-runtime-test"
    if app.exists():
        if not marker.is_file() or marker.read_text() != TITLE:
            raise ValueError(f"Refusing to replace an unowned title folder: {app}")
        extras = [str(p.relative_to(app)) for p in app.rglob("*") if p.is_file() and p.relative_to(app).as_posix() not in expected]
        if extras:
            raise ValueError(f"Unexpected files in the test package: {extras}")
    app.mkdir(parents=True, exist_ok=True)
    marker.write_text(TITLE)
    for name, source in copies.items():
        destination = app / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    subprocess.run([str(args.tool), "self", "--sign", "--in", str(args.build / "eboot.elf"),
                    "--out", str(app / "eboot.bin"), "--magic", "0x1D3D154F"], check=True)
    check_fself(app / "eboot.bin")
    metadata = json.loads((app / "sce_sys/param.json").read_text())
    if metadata["titleId"] != TITLE or metadata["localizedParameters"]["en-US"]["titleName"] != "Pokopia Runtime Tests":
        raise ValueError("Unexpected test app identity")
    with (args.build / "fself-inspection.txt").open("w") as output:
        subprocess.run([str(args.tool), "self", "--inspect", "--file", str(app / "eboot.bin")], stdout=output, check=True)
    suite = json.loads((args.build / "runtime-suite.json").read_text())
    manifest = {"title_id": TITLE, **suite, "context": "native-app",
                "gameplay_verified": False,
                "toolchain_revision": subprocess.check_output(["git", "-C", str(args.template), "rev-parse", "HEAD"], text=True).strip(),
                "files": {name: {"bytes": (app / name).stat().st_size, "sha256": sha(app / name)} for name in sorted(expected)}}
    (args.build / "native-app-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Native runtime test package: {app}")
    if suite.get("executes_game_sdk"):
        detail = "this diagnostic includes rtld and SDK code, mapping the SDK stack and running its first worker to a guarded mutex request."
    elif suite.get("executes_game_rtld"):
        detail = "this diagnostic includes the recompiled rtld and stops at the SDK handoff."
    else:
        detail = "this package contains no Pokopia game code or assets."
    print("Launch through ShadowMountPlus; " + detail)


if __name__ == "__main__":
    main()
