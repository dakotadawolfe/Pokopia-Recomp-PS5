#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run bounded PS5 diagnostics and retain receipts without claiming gameplay proof.

Requires the user's running Payload Manager and an explicitly hashed ftpsrv ELF.
This tool owns only the helper and payload slots it creates in its session.
"""
import argparse
import ftplib
import hashlib
import json
from pathlib import Path
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Console:
    def __init__(self, args):
        self.args = args
        self.state_path = args.state.resolve()
        self.base = f"http://{args.host}:{args.port}"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else None
        if self.state and (self.state["host"], self.state["port"], self.state["ftp_port"]) != (args.host, args.port, args.ftp_port):
            raise ValueError("The receipt belongs to another endpoint")

    def request(self, path, data=None, length=None):
        headers = {"X-PLDMGR-Request": "1", "Origin": self.base}
        if length is not None:
            headers["Content-Length"] = str(length)
        request = urllib.request.Request(self.base + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            detail = error.read(4096).decode(errors="replace")
            raise RuntimeError(f"Payload Manager {error.code}: {detail}") from error

    def processes(self):
        return json.loads(self.request("/processes_list"))["processes"]

    def slots(self):
        return json.loads(self.request("/list_payloads"))["items"]

    def save(self):
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(self.state, indent=2) + "\n")

    def active(self):
        if not self.state or self.state.get("closed"):
            raise RuntimeError("Open an owned helper session first")

    def ftp_open(self):
        ftp = ftplib.FTP()
        ftp.connect(self.args.host, self.args.ftp_port, timeout=30)
        ftp.login()
        return ftp

    def ftp_listening(self):
        try:
            with socket.create_connection((self.args.host, self.args.ftp_port), timeout=2):
                return True
        except (ConnectionRefusedError, TimeoutError, socket.timeout):
            return False

    def upload(self, path, purpose):
        self.active()
        path = path.resolve(strict=True)
        # Payload Manager v0.6.3 bounds HTTP uploads at 512 MiB. Larger
        # recompiled programs can be staged through the owned FTP helper.
        if path.stat().st_size > 512 * 1024 * 1024:
            raise ValueError("Payload exceeds Payload Manager's 512 MiB upload limit; use --transport ftp")
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise ValueError("Payload input is not an ELF")
        sha = digest(path)
        # No hyphen/version suffix: Payload Manager normalizes those into slots
        # and can turn distinct hexadecimal prefixes into the same slot name.
        filename = f"pkp{self.state['token']}{purpose}{uuid.uuid4().hex[:8]}.elf"
        before = {item["slot"] for item in self.slots()}
        upload_error = None
        self.state["pending_upload"] = {"filename": filename, "previous_slots": sorted(before)}
        self.save()
        print(f"Uploading {path.name}: {path.stat().st_size} bytes, SHA-256 {sha}", flush=True)
        try:
            with path.open("rb") as stream:
                self.request("/manage:upload?" + urllib.parse.urlencode({"filename": filename, "mode": "new"}), stream, path.stat().st_size)
        except (RuntimeError, OSError) as error:
            upload_error = error
        # An HTTP failure can follow a successful mutation. Discover and record
        # ownership before returning the error; never blindly repeat an upload.
        found = [item for item in self.slots() if item["filename"] == filename]
        if len(found) == 1 and found[0]["slot"] not in before:
            self.state["owned_slots"].append(found[0]["slot"])
            self.state.pop("pending_upload", None)
            self.save()
        if upload_error:
            raise upload_error
        if len(found) != 1 or found[0]["slot"] in before:
            raise RuntimeError("Upload did not produce a new, uniquely owned slot")
        return found[0], sha

    def load(self, item):
        result = self.request("/loadpayload:" + urllib.parse.quote(item["path"], safe=""), b"")
        if result.strip() != b"OK":
            raise RuntimeError(f"Unexpected load response: {result!r}")

    def stage_payload(self, path):
        self.active()
        path = path.resolve(strict=True)
        with path.open("rb") as source:
            if source.read(4) != b"\x7fELF":
                raise ValueError("Payload input is not an ELF")
        sha = digest(path)
        directory = f"/data/pldmgr/pkp{self.state['token']}"
        filename = f"probe{uuid.uuid4().hex}.elf"
        remote = f"{directory}/{filename}"
        with self.ftp_open() as ftp:
            ftp.cwd("/data/pldmgr")
            entries = {name for name, _ in ftp.mlsd()}
            if directory.rsplit("/", 1)[1] in entries:
                if directory not in self.state.get("owned_directories", []):
                    raise RuntimeError("Diagnostic staging directory already exists without ownership")
            else:
                self.state.setdefault("owned_directories", []).append(directory)
                self.save()
                ftp.mkd(directory)
            self.state.setdefault("owned_files", []).extend([remote + ".part", remote])
            self.save()
            print(f"Staging diagnostic {path.name}: {path.stat().st_size} bytes, SHA-256 {sha}", flush=True)
            with path.open("rb") as source:
                ftp.storbinary("STOR " + remote + ".part", source, blocksize=1024 * 1024)
            actual = hashlib.sha256()
            ftp.retrbinary("RETR " + remote + ".part", actual.update, blocksize=1024 * 1024)
            if actual.hexdigest() != sha:
                raise RuntimeError("Staged diagnostic failed complete readback SHA-256 verification")
            ftp.rename(remote + ".part", remote)
        print("Diagnostic staged and readback-verified", flush=True)
        return {"path": remote}, sha

    def open(self):
        if self.state and not self.state.get("closed"):
            raise RuntimeError("An existing session needs inspection/cleanup first")
        if digest(self.args.helper) != self.args.sha256.lower():
            raise ValueError("Transfer helper differs from its explicit expected SHA-256")
        if self.ftp_listening():
            raise RuntimeError("A pre-existing FTP server is running; it will not be replaced")
        self.state = {"host": self.args.host, "port": self.args.port, "ftp_port": self.args.ftp_port,
                      "token": uuid.uuid4().hex[:12], "baseline_processes": self.processes(),
                      "baseline_slots": self.slots(), "owned_slots": [], "runs": [], "closed": False}
        self.save()
        item, _ = self.upload(self.args.helper, "transfer")
        self.load(item)
        baseline = {process["pid"] for process in self.state["baseline_processes"]}
        for _ in range(30):
            helpers = [process for process in self.processes() if process["pid"] not in baseline and process["name"] == "ftpsrv.elf"]
            if len(helpers) == 1:
                self.state["helper_pid"] = helpers[0]["pid"]
                self.save()
                with self.ftp_open() as ftp:
                    print(f"Owned FTP helper PID {helpers[0]['pid']}; root {ftp.pwd()}", flush=True)
                return
            time.sleep(0.3)
        raise RuntimeError("Could not uniquely identify the new transfer helper")

    def names(self, ftp):
        ftp.cwd("/data/pokopia-recomp/tests")
        return {name for name, _ in ftp.mlsd() if re.fullmatch(r"[A-Za-z0-9_-]+-\d+-\d+\.log", name)}

    def read_log(self, ftp, name):
        if name not in self.names(ftp):
            raise ValueError("Report name is not a known diagnostic log")
        local = self.state_path.parent / name
        with local.open("wb") as output:
            ftp.retrbinary("RETR " + name, output.write)
        return local

    def collect(self):
        self.active()
        with self.ftp_open() as ftp:
            names = sorted((name for name in self.names(ftp) if name.startswith(self.args.prefix)),
                           key=lambda name: tuple(map(int, name[:-4].rsplit("-", 2)[1:])))
            if not names:
                raise RuntimeError("No matching diagnostic report")
            for name in names[-self.args.last:]:
                path = self.read_log(ftp, name)
                print(f"{path}\n{path.read_text(errors='replace')}")

    def run(self):
        self.active()
        with self.ftp_open() as ftp:
            previous = self.names(ftp)
        if self.args.transport == "ftp":
            item, sha = self.stage_payload(self.args.payload)
        else:
            item, sha = self.upload(self.args.payload, "probe")
        self.state["pending_launch"] = {"path": item["path"], "sha256": sha,
                                         "prefix": self.args.prefix, "baseline_processes": self.processes()}
        self.save()
        self.load(item)
        start = time.monotonic()
        while time.monotonic() - start < self.args.timeout:
            with self.ftp_open() as ftp:
                names = sorted(name for name in self.names(ftp) - previous if name.startswith(self.args.prefix))
                if names:
                    if len(names) != 1:
                        raise RuntimeError("More than one new matching report; inspect before claiming ownership")
                    name = names[0]
                    path = self.read_log(ftp, name)
                    pid = int(name[:-4].rsplit("-", 1)[1])
                    self.state["pending_run"] = {"pid": pid, "log": name, "sha256": sha}
                    self.save()
                    print(f"Report {name}: {path.stat().st_size} bytes", flush=True)
                    if pid not in {process["pid"] for process in self.processes()}:
                        path = self.read_log(ftp, name)
                        self.state["runs"].append({"payload": str(self.args.payload.resolve()), "sha256": sha,
                                                   "log": name, "pid": pid, "process_exited": True})
                        self.state.pop("pending_run", None)
                        self.state.pop("pending_launch", None)
                        self.save()
                        print(path.read_text(errors="replace")[-24000:])
                        return
            time.sleep(3)
        raise RuntimeError("Diagnostic did not finish within the collection limit; inspect its process and report")

    def close(self):
        self.active()
        pid = self.state.get("helper_pid")
        if not pid:
            raise RuntimeError("Helper ownership was not established; inspect the session before cleanup")
        current = {process["pid"]: process for process in self.processes()}
        pending = self.state.get("pending_run")
        if pending and pending["pid"] in current:
            raise RuntimeError(f"Diagnostic PID {pending['pid']} is still running; inspect before closing its transfer helper")
        if self.state.get("pending_launch"):
            raise RuntimeError("A diagnostic launch has no verified completion; inspect its process/report before cleanup")
        reused_pid = pid in current and current[pid]["name"] != "ftpsrv.elf"
        if reused_pid:
            if self.ftp_listening():
                raise RuntimeError("Helper PID was reused and another FTP server is present; refusing to close it")
            # After a reboot the old helper can be gone while its PID belongs
            # to a different program. Preserve that process and clean only the
            # payload slots recorded as ours, with the transfer port closed.
            self.state["helper_pid_reused"] = {"pid": pid, "current_name": current[pid]["name"]}
            self.save()
        pending_upload = self.state.get("pending_upload")
        if pending_upload:
            found = [item for item in self.slots() if item["filename"] == pending_upload["filename"]]
            if len(found) > 1 or any(item["slot"] in pending_upload["previous_slots"] for item in found):
                raise RuntimeError("Ambiguous ownership after interrupted upload; inspect before deleting")
            if found:
                self.state["owned_slots"].append(found[0]["slot"])
            self.state.pop("pending_upload")
            self.save()
        if self.state.get("owned_directories"):
            expected = f"/data/pldmgr/pkp{self.state['token']}"
            if self.state["owned_directories"] != [expected] or any(
                    not re.fullmatch(re.escape(expected) + r"/probe[0-9a-f]{32}\.elf(?:\.part)?", name)
                    for name in self.state.get("owned_files", [])):
                raise RuntimeError("Unexpected diagnostic staging ownership; refusing cleanup")
            with self.ftp_open() as ftp:
                ftp.cwd(expected)
                names = {name for name, _ in ftp.mlsd()}
                for remote in self.state.get("owned_files", []):
                    if remote.rsplit("/", 1)[1] in names:
                        ftp.sendcmd("DELE " + remote)
                if any(name not in (".", "..") for name, _ in ftp.mlsd()):
                    raise RuntimeError("Diagnostic staging directory is not empty after scoped cleanup")
                ftp.cwd("/data/pldmgr")
                ftp.rmd(expected)
                if expected.rsplit("/", 1)[1] in {name for name, _ in ftp.mlsd()}:
                    raise RuntimeError("Diagnostic staging directory remains after cleanup")
            self.state["owned_directories"] = []
            self.state["owned_files"] = []
            self.save()
        if pid in current and not reused_pid:
            with self.ftp_open() as ftp:
                try:
                    ftp.sendcmd("KILL")
                except (EOFError, ConnectionResetError):
                    pass
            for _ in range(30):
                if pid not in {process["pid"] for process in self.processes()}:
                    break
                time.sleep(0.3)
        current = {process["pid"]: process for process in self.processes()}
        if (pid in current and current[pid]["name"] == "ftpsrv.elf") or self.ftp_listening():
            raise RuntimeError("Transfer helper cleanup remains incomplete")
        failures = []
        for slot in self.state["owned_slots"]:
            if slot in {item["slot"] for item in self.slots()}:
                try:
                    self.request("/manage:delete?" + urllib.parse.urlencode({"slot": slot}), b"")
                except RuntimeError as error:
                    failures.append(str(error))
        remaining = set(self.state["owned_slots"]) & {item["slot"] for item in self.slots()}
        if remaining:
            raise RuntimeError(f"Owned slots still present: {sorted(remaining)}; {failures}")
        self.state.update(closed=True, final_processes=self.processes(), final_slots=self.slots())
        self.save()
        print("Owned helper and payload slots removed; FTP port closed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=8084)
    parser.add_argument("--ftp-port", type=int, default=2121)
    parser.add_argument("--state", type=Path, required=True)
    actions = parser.add_subparsers(dest="action", required=True)
    opening = actions.add_parser("open")
    opening.add_argument("--helper", type=Path, required=True)
    opening.add_argument("--sha256", required=True)
    collect = actions.add_parser("collect")
    collect.add_argument("--prefix", required=True)
    collect.add_argument("--last", type=int, default=1)
    run = actions.add_parser("run")
    run.add_argument("--payload", type=Path, required=True)
    run.add_argument("--prefix", required=True)
    run.add_argument("--timeout", type=int, default=150)
    run.add_argument("--transport", choices=("manager", "ftp"), default="manager")
    actions.add_parser("close")
    args = parser.parse_args()
    getattr(Console(args), args.action)()


if __name__ == "__main__":
    main()
