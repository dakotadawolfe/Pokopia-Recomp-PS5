#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Merge a verified cache with a translated batch, preserving binding metadata.

All inputs and outputs are private game-derived data. Never add them to Git.
The previous manifest is the manifest produced when that cache was prepared.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def identity(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": digest}


def checked_name(name):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or name in (".", ".."):
        raise ValueError(f"Unsafe cache filename: {name!r}")
    return name


def prepare(previous, previous_manifest, addition, selection, output, validator, replace_existing=False, hardlink=False):
    previous, addition = previous.resolve(strict=True), addition.resolve(strict=True)
    output = output.resolve()
    if output.exists() or any(root == output or root in output.parents for root in (previous, addition)):
        raise ValueError("Use a new output directory outside the input caches")
    old = json.loads(previous_manifest.read_text())
    for name, expected in old["files"].items():
        if identity(previous / checked_name(name)) != expected:
            raise ValueError(f"Previous cache differs from its manifest: {name}")
    rows, files, columns, replaced = {}, {}, None, []
    for root in (previous, addition):
        with (root / "results.tsv").open(newline="") as stream:
            reader = csv.DictReader(stream, delimiter="\t")
            if columns is not None and reader.fieldnames != columns:
                raise ValueError("Shader metadata schemas differ")
            columns = reader.fieldnames
            for row in reader:
                key = row["hash"]
                if not re.fullmatch(r"[0-9a-f]{16}", key) or row["status"] != "ok" or row["error"]:
                    raise ValueError(f"Unvalidated shader row: {key}")
                path = root / (key + ".spv")
                if path.stat().st_size != int(row["spv_bytes"]):
                    raise ValueError(f"Shader size differs from translation result: {key}")
                subprocess.run([str(validator), "--target-env", "vulkan1.3", str(path)], check=True)
                if key in rows:
                    semantic = lambda r: {k: v for k, v in r.items() if k != "ms"}
                    if identity(path) != identity(files[key]) or semantic(row) != semantic(rows[key]):
                        if not replace_existing:
                            raise ValueError(f"Conflicting shader or binding metadata: {key}")
                        replaced.append(key)
                        rows[key], files[key] = row, path
                else:
                    rows[key], files[key] = row, path
    if not rows:
        raise ValueError("Empty shader cache")
    output.mkdir(parents=True)
    for key, path in files.items():
        if hardlink:
            os.link(path, output / (key + ".spv"))
        else:
            shutil.copyfile(path, output / (key + ".spv"))
    with (output / "results.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows[key] for key in sorted(rows))
    provenance = {"previous_manifest_sha256": identity(previous_manifest)["sha256"],
                  "addition_results_sha256": identity(addition / "results.tsv")["sha256"],
                  "selection": json.loads(selection.read_text()), "retranslated_shaders": replaced}
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    scope = ("startup-and-observed-source-files" if provenance["selection"]["scope"] ==
             "runtime-misses-and-source-files" else "startup-and-observed-source-entries")
    manifest = {"scope": scope, "full_game_coverage": False,
                "blocks": len(rows), "validation": "spirv-val --target-env vulkan1.3",
                "files": {p.name: identity(p) for p in sorted(output.iterdir())}}
    (output / "shader-cache-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared {len(rows)} validated shaders with binding metadata at {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("previous_cache", "previous_manifest", "addition", "selection", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--spirv-val", type=Path, required=True)
    parser.add_argument("--replace-existing", action="store_true", help="Replace changed translations after a compiler correction, recording every replaced key")
    parser.add_argument("--hardlink", action="store_true", help="Link immutable SPIR-V inputs on the same filesystem to avoid duplicating large private caches; never edit linked files in place")
    args = parser.parse_args()
    prepare(args.previous_cache, args.previous_manifest, args.addition,
            args.selection, args.output, args.spirv_val, args.replace_existing, args.hardlink)
