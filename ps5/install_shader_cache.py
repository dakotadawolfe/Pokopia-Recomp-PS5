#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace the owned, stopped graphics test's shader cache with full readback.

Requires an open transfer helper and a previous successful installation receipt.
The existing directory is preserved as a uniquely named backup. Never launches
the app or changes game assets, saves, or another title's cache.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import uuid

from prepare_shader_cache import checked_name, identity

DESTINATION = "/data/pokopia-recomp/shader_cache/spirv"
TITLE = "/data/homebrew/PPSA98732"


def verify_remote(ftp, directory, files):
    entries = {name: facts for name, facts in ftp.mlsd(directory) if name not in (".", "..")}
    if set(entries) != set(files) or any(f.get("type") != "file" for f in entries.values()):
        raise ValueError(f"Unexpected contents in {directory}; preserving them")
    for name, expected in files.items():
        digest, size = hashlib.sha256(), 0
        def receive(data):
            nonlocal size
            size += len(data)
            digest.update(data)
        ftp.retrbinary("RETR " + directory + "/" + checked_name(name), receive, blocksize=1024*1024)
        if {"bytes": size, "sha256": digest.hexdigest()} != expected:
            raise ValueError(f"Remote cache identity differs: {directory}/{name}")


def activate(ftp, staging, backup):
    ftp.rename(DESTINATION, backup)
    try:
        ftp.rename(staging, DESTINATION)
    except Exception:
        ftp.rename(backup, DESTINATION)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--previous-receipt", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.cache / "shader-cache-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    files = dict(manifest["files"])
    files[manifest_path.name] = identity(manifest_path)
    if args.receipt.exists() or {p.name for p in args.cache.iterdir()} != set(files):
        raise ValueError("Receipt exists or candidate cache contains unexpected files")
    for name, expected in files.items():
        if identity(args.cache / checked_name(name)) != expected:
            raise ValueError(f"Local cache differs from its manifest: {name}")
    previous = json.loads(args.previous_receipt.read_text())
    if (previous.get("host") != args.host or previous.get("destination") != DESTINATION or
            previous.get("installed") is not True):
        raise ValueError("Previous receipt is not a successful installation on this console")
    for name in previous["files"]:
        checked_name(name)

    def stopped():
        request = urllib.request.Request(f"http://{args.host}:10101/api/v1/games/info",
            data=b'{"title_id":"PPSA98732"}', headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=20) as response:
            state = json.load(response)
        if (state.get("title_id") != "PPSA98732" or state.get("path") != TITLE or
                not state.get("managed") or not state.get("source_available") or state.get("mounted") is not False):
            raise ValueError("Owned graphics title is unavailable or still mounted")

    stopped()
    suffix = uuid.uuid4().hex
    receipt = {"host": args.host, "destination": DESTINATION,
               "staging": DESTINATION + "." + suffix + ".part",
               "backup": DESTINATION + "." + suffix + ".previous",
               "manifest_sha256": identity(manifest_path)["sha256"],
               "previous_receipt_sha256": identity(args.previous_receipt)["sha256"],
               "scope": manifest["scope"], "files": files, "installed": False}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    def save():
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")

    with ftplib.FTP() as ftp:
        ftp.connect(args.host, 2121, timeout=60)
        ftp.login()
        marker = io.BytesIO()
        ftp.retrbinary("RETR " + TITLE + "/.pokopia-graphics-test", marker.write)
        if marker.getvalue() != b"PPSA98732":
            raise ValueError("Title ownership marker differs")
        verify_remote(ftp, DESTINATION, previous["files"])
        save()
        ftp.mkd(receipt["staging"])
        for index, name in enumerate(sorted(files), 1):
            with (args.cache / name).open("rb") as stream:
                ftp.storbinary("STOR " + receipt["staging"] + "/" + name, stream, blocksize=1024*1024)
            if index % 100 == 0:
                print(f"Staged {index}/{len(files)} cache files", flush=True)
        verify_remote(ftp, receipt["staging"], files)
        receipt["staging_verified"] = True
        save()
        stopped()
        activate(ftp, receipt["staging"], receipt["backup"])
        receipt["renamed"] = True
        save()
        verify_remote(ftp, DESTINATION, files)
        receipt["installed"] = True
        save()
        print(f"Installed {manifest['blocks']} shaders; full readback verified and previous cache preserved", flush=True)


if __name__ == "__main__":
    main()
