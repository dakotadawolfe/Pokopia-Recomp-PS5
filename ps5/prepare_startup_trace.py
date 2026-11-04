#!/usr/bin/env python3
"""Bind a private startup diagnostic to verified local NSOs and module archives."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from check_modules import ABI, digest

ORDER = ("main", "sdk", "subsdk0", "rtld")


def prepare(manifest_path, build, output):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    report_path = build / "module-verification.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("abi") != ABI:
        raise ValueError("Module verification report has the wrong ABI")
    cache_path = build / "CMakeCache.txt"
    cache = cache_path.read_text(encoding="utf-8")
    match = re.search(r"^PKP_INPUT:PATH=(.+)$", cache, re.M)
    if not match:
        raise ValueError("Module build does not identify its decrypted input")
    # Validate the module build's recorded input against the independently
    # prepared image manifest, including any alternate spelling of its path.
    from validate_input import inspect
    source = inspect(Path(match.group(1)).resolve())
    entries = {entry["name"]: entry for entry in manifest["modules"]}
    if set(entries) != set(ORDER):
        raise ValueError("Startup requires exactly main, sdk, subsdk0 and rtld")
    records = []
    for index, name in enumerate(ORDER):
        entry = entries[name]
        module = report["modules"][name]
        archive = build / f"libpkp_{name}.a"
        verified = report["archives"][archive.name]
        archive_hash = digest(archive)
        if archive.stat().st_size != verified["bytes"] or archive_hash != verified["sha256"]:
            raise ValueError(f"{archive.name} changed since module verification; rerun check_modules.py")
        if module["abi"] != ABI or module["untranslated_units"] or module["function_entries"] <= 0:
            raise ValueError(f"{name} is not a verified translated ABI {ABI} module")
        source_hash = digest(Path(entry["source"]))
        if source_hash != entry["sha256"] or source_hash != source["module_sha256"][name]:
            raise ValueError(f"{name} input does not match the module build and image manifest")
        records.append({"name": name, "index": index, "base": entry["base"], "abi": ABI,
                        "functions": module["function_entries"], "source_sha256": source_hash,
                        "archive_sha256": archive_hash, "archive_bytes": verified["bytes"]})
    program_id = int(source["program_id"], 16)
    stamp = {"schema": 1, "purpose": "bounded native game-startup trace", "abi": ABI,
             "program_id": source["program_id"], "version": source["version"],
             "main_build_id": manifest["main_build_id"], "modules": records,
             "image_manifest_sha256": digest(manifest_path), "module_report_sha256": digest(report_path),
             "module_build_cache_sha256": digest(cache_path), "executes_guest_code": True,
             "gameplay_verified": False}
    encoded = (json.dumps(stamp, indent=2) + "\n").encode()
    stamp_hash = hashlib.sha256(encoded).hexdigest()
    declarations = [f'    {{"{r["name"]}", {r["index"]}u, {r["base"]}ull, {r["abi"]}u, '
                    f'{r["functions"]}, "{r["source_sha256"]}", "{r["archive_sha256"]}"}}'
                    for r in records]
    header = """// Private local input stamp; never commit generated output.
#pragma once
#include <cstdint>
struct StartupModuleInput {
    const char* name; uint32_t index; uint64_t base; uint32_t abi; int functions;
    const char* source_sha256; const char* archive_sha256;
};
inline constexpr StartupModuleInput startup_module_inputs[] = {
""" + ",\n".join(declarations) + "\n};\n" + f'inline constexpr char startup_input_stamp[] = "{stamp_hash}";\n'
    header += f'inline constexpr uint64_t startup_program_id = {program_id}ull;\n'
    header += f'inline constexpr char startup_version[] = {json.dumps(source["version"])};\n'
    output.mkdir(parents=True, exist_ok=True)
    (output / "startup-inputs.json").write_bytes(encoded)
    (output / "private_startup_inputs.h").write_text(header, encoding="utf-8")
    print(f"Verified four local startup archives and input identities; private stamp {stamp_hash}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("module_build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.manifest.resolve(), args.module_build.resolve(), args.output.resolve())
