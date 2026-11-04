#!/usr/bin/env python3
"""Verify a decrypted dump and describe its input layers without changing it."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def candidates(root):
    if root.name.lower() == "exefs":
        return [root]
    found = [root / "exefs"]
    updates = root / "updates"
    if updates.is_dir():
        versions = sorted((p for p in updates.iterdir() if p.is_dir()),
                          key=lambda p: (int(p.name) if p.name.isdigit() else -1, p.name),
                          reverse=True)
        found.extend(p / "exefs" for p in versions)
    found.append(root / "base" / "exefs")
    return [p for p in found if p.is_dir()]


def inspect(root):
    expected = json.loads((REPO / "game-build/release_b/dump_hashes.json").read_text(encoding="utf-8"))
    if not root.is_dir():
        raise ValueError(f"Input directory does not exist: {root}. Use --input for the existing dump.")
    errors = []
    selected = None
    hashes = {}
    for exefs in candidates(root):
        missing = [name for name in expected["sha256"] if not (exefs / name).is_file()]
        if missing:
            errors.append(f"{exefs}: missing {', '.join(missing)}")
            continue
        with (exefs / "main").open("rb") as stream:
            header = stream.read(0x60)
        if header[:4] != b"NSO0" or header[0x40:0x54].hex() != expected["main_build_id"]:
            errors.append(f"{exefs}: unsupported executable build")
            continue
        hashes = {name: sha256(exefs / name) for name in expected["sha256"]}
        mismatched = [name for name, digest in hashes.items() if digest != expected["sha256"][name]]
        if mismatched:
            errors.append(f"{exefs}: hash mismatch for {', '.join(mismatched)}")
            continue
        selected = exefs
        break
    if selected is None:
        raise ValueError("No supported decrypted executable set found.\n" + "\n".join(errors))

    # An update may overlay base assets. Keep both roots instead of silently
    # assuming that an update-only RomFS is a complete game.
    game_root = selected.parent
    if selected.parent.parent.name == "updates":
        game_root = selected.parent.parent.parent
    layers = []
    for directory in (game_root / "base" / "romfs", selected.parent / "romfs"):
        directory = directory.resolve()
        if directory.is_dir() and directory not in layers:
            layers.append(directory)
    if not layers:
        raise ValueError(f"No RomFS assets found beside {selected} or in the base tree.")
    asset_layers = []
    for layer in layers:
        files = [p for p in layer.rglob("*") if p.is_file() and p.name != ".listing"]
        total = sum(p.stat().st_size for p in files)
        if not total:
            raise ValueError(f"RomFS layer contains no nonempty assets: {layer}")
        asset_layers.append({"path": str(layer), "files": len(files), "bytes": total})
    return {"schema": 1, "input_root": str(root), "exefs": str(selected),
            "program_id": expected["program_id"], "version": expected["version"],
            "main_build_id": expected["main_build_id"], "module_sha256": hashes,
            "romfs_layers": asset_layers, "asset_contents_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default="C:/Games/pokopia", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.input.resolve())
        report = REPO / "work/ps5/input-manifest.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError) as error:
        print(f"Input validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified {len(result['module_sha256'])} executable files for {result['version']}.")
    print(f"Build ID: {result['main_build_id']}")
    print(f"RomFS layers: {len(result['romfs_layers'])}; asset contents still require runtime validation.")
    print(f"Local input manifest: {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
