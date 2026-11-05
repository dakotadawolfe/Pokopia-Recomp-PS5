#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Select startup or observed missing shaders from the private Ampere corpus.

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


def prepare(source, output, runtime_log=None, expand_source_files=False, include_cache=None, max_shaders=10000):
    source, output = source.resolve(strict=True), output.resolve()
    if output.exists() or source in output.parents or output in source.parents:
        raise ValueError("Use a new, separate directory for the private startup corpus")
    with (source / "manifest.tsv").open() as stream:
        occurrences = list(csv.DictReader(stream, delimiter="\t"))
    with (source / "unique.tsv").open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        fields = reader.fieldnames
        unique = list(reader)
    observed = set()
    if expand_source_files and not runtime_log:
        raise ValueError("Source-file expansion requires an observed runtime log")
    if runtime_log:
        observed = set(re.findall(r"no usable [^\r\n]*/([0-9a-f]{16})\.spv", runtime_log.read_text()))
        indexed = {row["code_sha1_16"] for row in unique}
        if not observed or observed - indexed:
            raise ValueError("Runtime misses are empty or include shaders absent from the verified corpus")
        # Include the complete source entries of the observed blocks' first
        # occurrences. Expanding every shared occurrence would pull unrelated
        # level materials into a startup batch through common vertex shaders.
        entries = {(row["first_source"], row["entry_off"]) for row in unique
                   if row["code_sha1_16"] in observed}
        sources = {filename for filename, _ in entries}
        selected = observed | {row["code_sha1_16"] for row in occurrences
                               if (row["source"] in sources if expand_source_files else
                                   (row["source"], row["entry_off"]) in entries)}
    else:
        selected = {row["code_sha1_16"] for row in occurrences if row["container"] == "main"}
    if include_cache:
        cache = json.loads(include_cache.read_text())
        previous = {Path(name).stem for name in cache["files"] if re.fullmatch(r"[0-9a-f]{16}\.spv", name)}
        if len(previous) != cache["blocks"]:
            raise ValueError("Cache manifest shader count is inconsistent")
        selected |= previous
    if len(selected) > max_shaders:
        raise ValueError(f"Selection has {len(selected)} shaders, above the explicit limit {max_shaders}; narrow the scope before copying")
    rows = [row for row in unique if row["code_sha1_16"] in selected]
    if not selected or len(rows) != len(selected):
        raise ValueError("The shader selection is empty or its corpus index is inconsistent")
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
    scope = ("runtime-misses-and-source-files" if expand_source_files else
             "runtime-misses-and-source-entries" if runtime_log else "embedded-main-startup-only")
    stamp = {"scope": scope, "full_game_coverage": False,
             "unique_shaders": len(rows), "corpus_manifest_sha256": source_hash}
    if include_cache:
        stamp.update(included_cache_manifest_sha256=hashlib.sha256(include_cache.read_bytes()).hexdigest(),
                     included_cache_shaders=len(previous))
    if runtime_log:
        stamp.update(runtime_log_sha256=hashlib.sha256(runtime_log.read_bytes()).hexdigest(),
                     observed_missing_shaders=len(observed), source_entries=len(entries))
        if expand_source_files:
            stamp["source_files"] = sorted(sources)
    (output / "selection.json").write_text(json.dumps(stamp, indent=2) + "\n")
    print(f"Verified and selected {len(rows)} shaders ({stamp['scope']}); full-game coverage remains incomplete")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--runtime-log", type=Path, help="Cover logged cache misses and their source entries")
    parser.add_argument("--expand-source-files", action="store_true",
                        help="Include all shaders in the first-source files of observed misses")
    parser.add_argument("--include-cache", type=Path, help="Also regenerate every shader in this previous cache manifest")
    parser.add_argument("--max-shaders", type=int, default=10000, help="Refuse oversized selections before copying")
    args = parser.parse_args()
    prepare(args.corpus, args.output, args.runtime_log, args.expand_source_files, args.include_cache, args.max_shaders)
