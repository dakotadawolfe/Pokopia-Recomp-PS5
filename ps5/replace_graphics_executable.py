#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace only the owned, stopped PPSA98732 executable with verified readback.

Requires an already opened transfer helper. Records a backup and the unique
staging path before writing. Does not launch, register, or alter another title.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import uuid


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--file", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--previous-sha256", required=True)
    parser.add_argument("--libc-sha256", required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--install-shared-font", action="store_true", help="Install the repository's licensed DejaVu fallback and its license")
    parser.add_argument("--opus-license", type=Path, help="Built libopus license from the verified native package")
    parser.add_argument("--opus-license-sha256", help="Expected license hash from that package's manifest")
    args = parser.parse_args()
    if args.receipt.exists() or digest(args.file) != args.sha256:
        raise ValueError("Receipt exists or replacement hash differs")
    if bool(args.opus_license) != bool(args.opus_license_sha256):
        raise ValueError("Opus license and its expected hash must be supplied together")
    resources = []
    if args.opus_license:
        if digest(args.opus_license) != args.opus_license_sha256:
            raise ValueError("Opus license hash differs from the native package")
        resources.append(("licenses/opus-BSD-3-Clause.txt", args.opus_license))
    if args.install_shared_font:
        root = Path(__file__).resolve().parents[1]
        resources.extend((("fonts/DejaVuSans.ttf", root / "runtime/assets/fonts/DejaVuSans.ttf"),
                          ("licenses/DejaVu.txt", root / "runtime/assets/fonts/LICENSE-DejaVu.txt")))
    with args.file.open("rb") as source:
        if source.read(4) != bytes.fromhex("4f153d1d"):
            raise ValueError("Replacement is not a native FSELF")
    remote = "/data/homebrew/PPSA98732"

    def stopped():
        request = urllib.request.Request(f"http://{args.host}:10101/api/v1/games/info",
            data=b'{"title_id":"PPSA98732"}', headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=20) as response:
            state = json.load(response)
        if (state.get("title_id") != "PPSA98732" or state.get("path") != remote or
                not state.get("managed") or not state.get("source_available") or state.get("mounted") is not False):
            raise ValueError("Owned diagnostic title is unavailable or still mounted")

    stopped()
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    backup = args.receipt.with_suffix(".previous.bin")
    if backup.exists():
        raise ValueError("Backup already exists")
    receipt = {"host": args.host, "title_id": "PPSA98732", "source": str(args.file.resolve()),
               "expected_sha256": args.sha256, "previous_sha256": args.previous_sha256,
               "backup": str(backup.resolve()), "staging": remote + "/eboot." + uuid.uuid4().hex + ".part",
               "installed": False}
    def save():
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    with ftplib.FTP() as ftp:
        ftp.connect(args.host, 2121, timeout=60); ftp.login()
        if "disabled" not in ftp.sendcmd("SELF").lower():
            raise ValueError("FTP must return raw native executable bytes")
        marker = io.BytesIO()
        ftp.retrbinary("RETR " + remote + "/.pokopia-graphics-test", marker.write)
        if marker.getvalue() != b"PPSA98732":
            raise ValueError("Title ownership marker differs")
        libc_hash = hashlib.sha256()
        ftp.retrbinary("RETR " + remote + "/sce_module/libc.prx", libc_hash.update)
        if libc_hash.hexdigest() != args.libc_sha256:
            raise ValueError("Installed native libc differs from the verified build")
        if resources:
            receipt["resources"] = {}
            for relative, source in resources:
                target = remote + "/" + relative
                expected = digest(source)
                directory = target.rsplit("/", 1)[0]
                try:
                    ftp.cwd(directory)
                except ftplib.error_perm as error:
                    if not str(error).startswith("550"):
                        raise
                    ftp.mkd(directory)
                    ftp.cwd(directory)
                names = {name for name, _ in ftp.mlsd()}
                if target.rsplit("/", 1)[1] in names:
                    actual = hashlib.sha256()
                    ftp.retrbinary("RETR " + target, actual.update)
                    if actual.hexdigest() != expected:
                        raise ValueError(f"Existing resource differs; preserving {target}")
                else:
                    stage = target + "." + uuid.uuid4().hex + ".part"
                    receipt["resources"][relative] = {"sha256": expected, "staging": stage, "installed": False}; save()
                    with source.open("rb") as stream:
                        ftp.storbinary("STOR " + stage, stream)
                    actual = hashlib.sha256()
                    ftp.retrbinary("RETR " + stage, actual.update)
                    if actual.hexdigest() != expected:
                        raise ValueError(f"Resource staging readback differs: {relative}")
                    stopped()
                    ftp.rename(stage, target)
                actual = hashlib.sha256()
                ftp.retrbinary("RETR " + target, actual.update)
                if actual.hexdigest() != expected:
                    raise ValueError(f"Resource readback differs: {relative}")
                receipt["resources"][relative] = {"sha256": expected, "installed": True}; save()
            ftp.cwd("/")
        with backup.open("xb") as out:
            ftp.retrbinary("RETR " + remote + "/eboot.bin", out.write, blocksize=1024*1024)
        if digest(backup) != args.previous_sha256:
            raise ValueError("Installed executable differs; retained actual bytes for inspection")
        save()
        print(f"Uploading verified native executable: {args.file.stat().st_size} bytes", flush=True)
        with args.file.open("rb") as source:
            ftp.storbinary("STOR " + receipt["staging"], source, blocksize=1024*1024)
        staged = hashlib.sha256()
        ftp.retrbinary("RETR " + receipt["staging"], staged.update, blocksize=1024*1024)
        if staged.hexdigest() != args.sha256:
            raise ValueError("Staging readback differs; do not launch")
        stopped()
        ftp.rename(receipt["staging"], remote + "/eboot.bin")
        receipt["renamed"] = True; save()
        installed = hashlib.sha256()
        ftp.retrbinary("RETR " + remote + "/eboot.bin", installed.update, blocksize=1024*1024)
        if installed.hexdigest() != args.sha256:
            raise ValueError("Final executable readback differs; do not launch")
        receipt["installed"] = True; receipt["readback_sha256"] = installed.hexdigest(); save()
        print("Installed executable verified by complete SHA-256 readback", flush=True)


if __name__ == "__main__":
    main()
