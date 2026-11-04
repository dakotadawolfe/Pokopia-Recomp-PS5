#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Select embedded-main shaders from the existing private Ampere corpus.

This is startup coverage only, not the complete game shader set. The source
corpus is read-only; the destination must be new and stays outside Git.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil


def prepare(source, output):
    source, output = source.resolve(strict=True), output.resolve()
    if output.exists() or source in output.parents or output in source.parents:
        raise ValueError("Use a new, separate directory for the private startup corpus")
    with (source / "manifest.tsv").open() as stream:
        selected = {row["code_sha1_16"] for row in csv.DictReader(stream, delimiter="\t")
                    if row["container"] == "main"}
    with (source / "unique.tsv").open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        fields = reader.fieldnames
        rows = [row for row in reader if row["code_sha1_16"] in selected]
    if not selected or len(rows) != len(selected):
        raise ValueError("The corpus has no embedded-main shaders or its index is inconsistent")
    files = {}
    for row in rows:
        for folder, column in (("code", "code_sha1_16"), ("ctrl", "first_ctrl"), ("glslc", "first_glslc")):
            key = row[column]
            if not re.fullmatch(r"[0-9a-f]{16}", key):
                raise ValueError("Malformed shader content key")
            name = f"{folder}/{key}.bin"
            path = source / name
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha1").hexdigest()
            if digest[:16] != key:
                raise ValueError(f"Shader content differs from its index: {name}")
            files[name] = path
    output.mkdir(parents=True)
    for name, path in files.items():
        destination = output / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(path, destination)
    with (output / "unique.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with (source / "manifest.tsv").open("rb") as stream:
        source_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    stamp = {"scope": "embedded-main-startup-only", "full_game_coverage": False,
             "unique_shaders": len(rows), "corpus_manifest_sha256": source_hash}
    (output / "selection.json").write_text(json.dumps(stamp, indent=2) + "\n")
    print(f"Verified and selected {len(rows)} embedded-main shaders; full-game coverage remains incomplete")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.corpus, args.output)
