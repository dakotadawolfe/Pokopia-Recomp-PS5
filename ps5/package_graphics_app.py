#!/usr/bin/env python3
"""Package the standalone GPU display diagnostic; it contains no game assets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from package_runtime_app import check_fself

ROOT = Path(__file__).resolve().parents[1]
TITLE = "PPSA98732"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "template", "vulkan", "mesa", "tool"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    app = args.build / "package" / TITLE
    copies = {
        "sce_module/libc.prx": args.template / "runtime/libc.prx",
        "licenses/pokopia-runtime-GPL-3.0.txt": ROOT / "LICENSE",
        "licenses/native-toolchain-GPL-3.0.txt": args.template / "LICENSE",
        "licenses/vulkan-platform-GPL-3.0.txt": args.vulkan / "LICENSE",
        "licenses/mesa-license.rst": args.mesa / "docs/license.rst",
        "graphics-provenance.json": args.build / "graphics-provenance.json",
    }
    for name in ("icon0.png", "pic0.dds", "pic1.dds", "snd0.at9"):
        copies["sce_sys/" + name] = args.template / "sce_sys" / name
    expected = set(copies) | {"eboot.bin", "sce_sys/param.json", ".pokopia-graphics-test"}
    marker = app / ".pokopia-graphics-test"
    if app.exists():
        if not marker.is_file() or marker.read_text() != TITLE:
            raise ValueError(f"Refusing to replace an unowned title folder: {app}")
        extras = [str(path.relative_to(app)) for path in app.rglob("*")
                  if path.is_file() and path.relative_to(app).as_posix() not in expected]
        if extras:
            raise ValueError(f"Unexpected files in the graphics package: {extras}")
    app.mkdir(parents=True, exist_ok=True)
    marker.write_text(TITLE)
    for name, source in copies.items():
        destination = app / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    param = json.loads((ROOT / "ps5/app/param.json").read_text())
    param.update(titleId=TITLE, conceptId="98732", contentId="UP9000-PPSA98732_00-POKOPIAGRAPH0001")
    param["localizedParameters"]["en-US"]["titleName"] = "Pokopia Graphics Test"
    (app / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    subprocess.run([str(args.tool), "self", "--sign", "--in", str(args.build / "eboot.elf"),
                    "--out", str(app / "eboot.bin"), "--magic", "0x1D3D154F"], check=True)
    check_fself(app / "eboot.bin")
    with (args.build / "graphics-fself-inspection.txt").open("w") as output:
        subprocess.run([str(args.tool), "self", "--inspect", "--file", str(app / "eboot.bin")], stdout=output, check=True)
    provenance = json.loads((args.build / "graphics-provenance.json").read_text())
    manifest = {"title_id": TITLE, "context": "native-app", "diagnostic": "GPU quadrant clear/present/readback",
                "console_verified": False, **provenance,
                "files": {name: {"bytes": (app / name).stat().st_size, "sha256": sha(app / name)}
                          for name in sorted(expected)}}
    (args.build / "graphics-app-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Native graphics diagnostic package: {app}")
    print("Shows colored quadrants for about five seconds, verifies four pixels, then requests shell exit.")
    print("No game code/assets or guest mappings. Native display behavior still requires a console test.")


if __name__ == "__main__":
    main()
