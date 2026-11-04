#!/usr/bin/env python3
"""Package the native graphics or opt-in private game startup build, without assets."""
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
    provenance = json.loads((args.build / "graphics-provenance.json").read_text())
    game_mode = provenance.get("diagnostic_mode") == "game_renderer_startup"
    memory_mode = provenance.get("diagnostic_mode") == "guest_gpu_memory"
    runtime_mode = game_mode or provenance.get("diagnostic_mode") == "runtime_scene"
    nvn_mode = provenance.get("nvn_backend", provenance.get("nvn_replay_test", False))
    private_provenance = {}
    if game_mode:
        from package_startup_app import check_nonlocal
        check_nonlocal(args.build / "ps5_graphics_app", host_aliases=True)
        if not nvn_mode or not provenance.get("game_code") or not provenance.get("guest_exception_runtime"):
            raise ValueError("Game startup requires the real NVN and exception runtimes")
        private_provenance = {
            "input_stamp": json.loads((args.build / "private-startup/startup-inputs.json").read_text()),
            "nonlocal_jump_object_sha256": sha(args.build / "nxrt_ps5_nonlocal.o"),
            "exception_selftest_cases": 38,
            "host_nonlocal_selftest_cases": 1,
            "host_nonlocal_aliases_verified": True,
            "native_cpp_exception_runtime_verified": True,
            "startup_status_view": True,
            "startup_trace_mode": "first-svc-and-ipc-requests",
            "startup_profile_interval_seconds": 10,
            "report_buffering": "line",
        }
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
    if runtime_mode:
        for name in ("volk-MIT.txt", "VMA-MIT.txt", "Vulkan-Headers.txt", "Vulkan-Headers-Apache-2.0.txt", "Vulkan-Headers-MIT.txt"):
            copies["licenses/" + name] = args.build / "graphics-licenses" / name
    if nvn_mode:
        for name in ("astcenc-Apache-2.0.txt", "bc7enc-LICENSE.txt"):
            copies["licenses/" + name] = args.build / "graphics-licenses" / name
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
    diagnostic = "native folder picker and runtime shader/texture scene" if runtime_mode else "guest memory import, coherence and display" if memory_mode else "GPU quadrant clear/present/readback"
    if nvn_mode:
        diagnostic = "native picker, shader scene, NVN buffer/texture replay and ASTC/BC7 conversion"
    if game_mode:
        diagnostic = "native picker and recompiled game startup with the PS5 NVN renderer"
    manifest = {"title_id": TITLE, "context": "native-app", "diagnostic": diagnostic,
                "console_verified": False, **provenance, **private_provenance,
                "native_elf_sha256": sha(args.build / "eboot.elf"),
                "files": {name: {"bytes": (app / name).stat().st_size, "sha256": sha(app / name)}
                          for name in sorted(expected)}}
    (args.build / "graphics-app-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Native graphics diagnostic package: {app}")
    if game_mode:
        print("Runs verified private game modules against the native NVN renderer with a bounded startup deadline.")
        print("Uses the console folder picker; no game assets are copied. Gameplay is not yet verified.")
    elif runtime_mode:
        print("Selects and saves an existing console or USB game-folder location without copying assets.")
        print("Runs the real runtime Device/Renderer with animated textured quads and verified texture/mip samples.")
        if nvn_mode:
            print("Also exercises NVN buffer copies, target clears, presentation/readback and ASTC/BC7 conversion.")
        print("Guest windows remain reserved. No game code or game assets; console validation remains required.")
    elif memory_mode:
        print("Shows colored quadrants for about five seconds, verifies four pixels, then requests shell exit.")
        print("Tests actual guest address reservations and a guarded 16 KiB heap alias. No game code/assets.")
    else:
        print("Shows colored quadrants for about five seconds, verifies four pixels, then requests shell exit.")
        print("No game code/assets or guest mappings. Native display behavior still requires a console test.")


if __name__ == "__main__":
    main()
