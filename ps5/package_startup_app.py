#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a diagnostic executable for the already owned PPSA98732 app folder.

This is a temporary headless startup diagnostic, not a replacement user package.
Retain the installed graphics executable for restoration before deploying this.
No game inputs or complete app package are copied by this script.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from package_runtime_app import check_fself


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("template", type=Path)
    parser.add_argument("tool", type=Path)
    args = parser.parse_args()
    executable = args.build / "startup.bin"
    cache = (args.build / "CMakeCache.txt").read_text()
    threshold = [line.split("=", 1)[1] for line in cache.splitlines()
                 if line.startswith("PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB:STRING=")]
    if len(threshold) != 1 or not threshold[0].isdigit() or not 1 <= int(threshold[0]) <= 12288:
        raise ValueError("Missing bounded native heap threshold")
    optimization = [line.split("=", 1)[1] for line in cache.splitlines()
                    if line.startswith("NX_POSIX_OPT:STRING=")]
    if len(optimization) != 1 or optimization[0] not in ("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"):
        raise ValueError("Missing or invalid generated-code optimization setting")
    platform = [line.split("=", 1)[1] for line in cache.splitlines()
                if line.startswith("PKP_NATIVE_PLATFORM_ARCHIVE:FILEPATH=")]
    platform_sha = [line.split("=", 1)[1] for line in cache.splitlines()
                    if line.startswith("PKP_NATIVE_PLATFORM_SHA:STRING=")]
    if len(platform) != 1 or len(platform_sha) != 1 or digest(Path(platform[0])) != platform_sha[0]:
        raise ValueError("Native directory platform archive differs from the configured input")
    check_fself(executable)
    with (args.build / "startup-fself-inspection.txt").open("w") as output:
        subprocess.run([str(args.tool), "self", "--inspect", "--file", str(executable)], stdout=output, check=True)
    manifest = {"title_id": "PPSA98732", "context": "native-app", "diagnostic": "full-game-startup",
                "console_qualified": False,
                "uses_saved_selection": True, "renderer": False, "gameplay_verified": False,
                "generated_code_optimization": optimization[0],
                "directory_platform_sha256": platform_sha[0],
                "bootstrap_report": "/data/pokopia-recomp/tests/native-bootstrap.log",
                "guest_heap_stop_threshold_bytes": int(threshold[0]) << 20,
                "input_stamp": json.loads((args.build / "private-startup/startup-inputs.json").read_text()),
                "libc_sha256": digest(args.template / "runtime/libc.prx"),
                "native_elf_sha256": digest(args.build / "startup.elf"),
                "eboot": {"bytes": executable.stat().st_size, "sha256": digest(executable)}}
    (args.build / "startup-app-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified temporary startup executable: {executable}; SHA-256 {manifest['eboot']['sha256']}")


if __name__ == "__main__":
    main()
