#!/usr/bin/env python3
"""Inspect the compiled PS5 link inputs and record private build evidence."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from elftools.common.exceptions import ELFError

MODULES = {"main": 230, "sdk": 45, "subsdk0": 4, "rtld": 1}
ABI = 4
SDK_EH_ENTRIES = (0x750078f440, 0x750078f660, 0x750078f940, 0x750078f8a0, 0x750078f7e0)


def verify_eh_hooks(build, ar_tool):
    """Require the hook in emitted code and a call relocation in each compiled function."""
    found = {}
    for unit in sorted((build / "lift/sdk").glob("c_*.cpp")):
        source = unit.read_text(encoding="utf-8")
        addresses = [a for a in SDK_EH_ENTRIES if f"if (nx_hook_entry(c, 0x{a:x}ULL)) return;" in source]
        if not addresses:
            continue
        elf = ELFFile(io.BytesIO(ar(ar_tool, "p", build / "libpkp_sdk.a", unit.name + ".o")))
        symbols = elf.get_section_by_name(".symtab")
        for address in addresses:
            functions = [s for s in symbols.iter_symbols() if f"f_{address:x}" in s.name
                         and s["st_info"]["type"] == "STT_FUNC"]
            if len(functions) != 1 or address in found:
                raise ValueError(f"Missing or duplicate compiled EH entry {address:x}")
            function = functions[0]
            calls = 0
            for section in elf.iter_sections():
                if not isinstance(section, RelocationSection) or section["sh_info"] != function["st_shndx"]:
                    continue
                table = elf.get_section(section["sh_link"])
                for relocation in section.iter_relocations():
                    offset = relocation["r_offset"]
                    if function["st_value"] <= offset < function["st_value"] + function["st_size"]:
                        if table.get_symbol(relocation["r_info_sym"]).name == "nx_hook_entry":
                            calls += 1
            if calls != 1:
                raise ValueError(f"Compiled EH entry {address:x} has {calls} hook calls, expected one")
            found[address] = unit.name
    if set(found) != set(SDK_EH_ENTRIES):
        raise ValueError("SDK exception entry hooks missing; rebuild modules with sdk_exception.sites")
    return [hex(a) for a in SDK_EH_ENTRIES]


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def ar(ar_tool, *args):
    return subprocess.run([ar_tool, *map(str, args)], check=True, capture_output=True).stdout


def inspect(build, ar_tool):
    result = {"abi": ABI, "archives": {}, "modules": {}}
    for name in [*("pkp_" + name for name in MODULES), "nxrt_ps5_core", "nxrt_ps5_platform", "nxrt_ps5_kernel", "nxrt_ps5_svc"]:
        archive = build / ("lib" + name + ".a")
        members = ar(ar_tool, "t", archive).decode().splitlines()
        if not members:
            raise ValueError(f"Empty archive: {archive}")
        result["archives"][archive.name] = {
            "bytes": archive.stat().st_size, "sha256": digest(archive), "members": len(members)}
        if not name.startswith("pkp_"):
            continue
        module = name[4:]
        member = f"module_{module}.cpp.o"
        if members.count(member) != 1 or len(members) != MODULES[module] + 1:
            raise ValueError(f"Unexpected members in {archive.name}")
        elf = ELFFile(io.BytesIO(ar(ar_tool, "p", archive, member)))
        if elf["e_machine"] != "EM_X86_64" or elf["e_type"] != "ET_REL" or not elf.little_endian:
            raise ValueError(f"Unexpected target in {archive.name}")
        symbols = elf.get_section_by_name(".symtab")
        entries = symbols.get_symbol_by_name(f"nxmod_{module}_abi") if symbols else None
        if not entries or len(entries) != 1:
            raise ValueError(f"Missing or duplicate module ABI in {archive.name}")
        symbol = entries[0]
        if symbol["st_size"] != 4 or not isinstance(symbol["st_shndx"], int):
            raise ValueError(f"Invalid ABI symbol in {archive.name}")
        section = elf.get_section(symbol["st_shndx"])
        offset = symbol["st_value"] - section["sh_addr"]
        data = section.data()
        if offset < 0 or offset + 4 > len(data):
            raise ValueError(f"ABI symbol outside its section in {archive.name}")
        abi = int.from_bytes(data[offset:offset + 4], "little")
        if abi != ABI:
            raise ValueError(f"{archive.name} uses ABI {abi}; PS5 requires checked-memory ABI {ABI}")

        directory = build / "lift" / module
        units = sorted(directory.glob("c_*.cpp"))
        if len(units) != MODULES[module]:
            raise ValueError(f"Unexpected generated unit count for {module}: {len(units)}")
        unsupported = [p.name for p in units if re.search(r"\bguest_unimpl\s*\(", p.read_text(encoding="utf-8"))]
        if unsupported:
            raise ValueError(f"Untranslated instructions in {module}: {unsupported}")
        functions = len(re.findall(r"^void f_[0-9a-fA-F]+\(Ctx\b", (directory / "funcs.h").read_text(encoding="utf-8"), re.M))
        descriptor = (directory / f"module_{module}.cpp").read_text(encoding="utf-8")
        count = re.search(r"\bfns,\s*(\d+),\s*syms", descriptor)
        if not count or functions != int(count.group(1)):
            raise ValueError(f"Generated function counts disagree for {module}")
        result["modules"][module] = {"abi": abi, "function_entries": functions,
                                     "translation_units": len(units), "untranslated_units": unsupported}
    result["sdk_exception_hooks"] = verify_eh_hooks(build, ar_tool)
    result["gameplay_verified"] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--ar", default="llvm-ar-18", help="LLVM archiver executable")
    parser.add_argument("--report", type=Path, help="Private JSON report; defaults inside the build directory")
    args = parser.parse_args()
    try:
        result = inspect(args.build.resolve(), args.ar)
        report = args.report or args.build / "module-verification.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, ELFError, subprocess.CalledProcessError) as error:
        print(f"Module verification failed: {error}", file=sys.stderr)
        return 1
    functions = sum(module["function_entries"] for module in result["modules"].values())
    print(f"Verified four ABI {ABI} game archives: {functions:,} discovered functions; no emitted guest_unimpl calls.")
    print(f"Build evidence: {report}. Game execution remains unverified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
