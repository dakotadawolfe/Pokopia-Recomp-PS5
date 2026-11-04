#!/usr/bin/env python3
"""Generate private loader expectations from a hash-verified decrypted dump."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

import lz4.block
from validate_input import inspect

MODULES = (("main", 0x7100000000), ("sdk", 0x7500000000),
           ("subsdk0", 0x7600000000), ("rtld", 0x7700000000))


def prepare(source, output):
    verified = inspect(source.resolve())
    exefs = Path(verified["exefs"])
    records, declarations = [], []
    for name, base in MODULES:
        path = exefs / name
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != verified["module_sha256"][name]:
            raise ValueError(f"{name} changed after input validation")
        header = data[:256]
        u32 = lambda offset: struct.unpack_from("<I", header, offset)[0]
        flags = u32(12)
        segments = []
        for index in range(3):
            offset, address, size = (u32(0x10 + index * 16 + field * 4) for field in range(3))
            stored_size = u32(0x60 + index * 4) if flags & (1 << index) else size
            stored = data[offset:offset + stored_size]
            plain = lz4.block.decompress(stored, uncompressed_size=size) if flags & (1 << index) else stored
            if len(plain) != size:
                raise ValueError(f"{name} segment {index}: wrong decompressed size")
            if not flags & (1 << (index + 3)) or hashlib.sha256(plain).digest() != header[0xA0 + index * 32:0xC0 + index * 32]:
                raise ValueError(f"{name} segment {index}: missing or incorrect NSO segment hash")
            segments.append({"offset": address, "bytes": size, "crc32": zlib.crc32(plain)})
        bss = u32(0x3C)
        record = {"name": name, "base": base, "source": str(path), "bytes": len(data),
                  "sha256": digest, "crc32": zlib.crc32(data), "segments": segments, "bss": bss}
        records.append(record)
        segment_text = ", ".join(f"{{{s['offset']}ull, {s['bytes']}ull, {s['crc32']}u}}" for s in segments)
        header_text = ", ".join(str(value) for value in header)
        declarations.append(f'    {{"{name}", {base}ull, {len(data)}ull, {record["crc32"]}u, '
                            f'{{{segment_text}}}, {bss}ull, {{{header_text}}}}}')
    output.mkdir(parents=True, exist_ok=True)
    header = """// Generated from verified local input. Keep outside Git.
#pragma once
#include <cstdint>
struct GameImageSegment { uint64_t offset, bytes; uint32_t crc32; };
struct GameImageInput {
    const char* name; uint64_t base, bytes; uint32_t crc32;
    GameImageSegment segments[3]; uint64_t bss; uint8_t header[256];
};
inline constexpr GameImageInput game_image_inputs[] = {
""" + ",\n".join(declarations) + "\n};\n"
    (output / "private_game_images.h").write_text(header, encoding="utf-8")
    manifest = {"schema": 1, "console_exefs": "/data/pokopia-recomp/game/exefs",
                "main_build_id": verified["main_build_id"], "modules": records,
                "executes_guest_code": False}
    (output / "game-image-inputs.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Verified NSO files and all 12 decompressed segment hashes; private expectations: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.input, args.output)
