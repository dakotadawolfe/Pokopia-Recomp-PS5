// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace nxrt::ps5_graphics {
inline uint64_t adrp_target(uint64_t pc, uint32_t instruction) {
    int64_t pages = ((instruction >> 29) & 3u) | (((instruction >> 5) & 0x7ffffu) << 2);
    if (pages & (1 << 20)) pages -= 1 << 21;
    return (pc & ~uint64_t{4095}) + static_cast<uint64_t>(pages * 4096);
}
inline uint64_t initialized_flag(uint64_t pc, std::span<const uint32_t> code) {
    if (pc % 4 || code.size() < 4 || (code[0] & 0x9f00001fu) != 0x90000008u ||
        (code[1] & 0xffc003ffu) != 0x91000108u || code[2] != 0xb8bfc108u || code[3] != 0x7100051fu)
        return 0;
    return adrp_target(pc, code[0]) + ((code[1] >> 10) & 4095u);
}
inline uint64_t driver_pointer(uint64_t pc, std::span<const uint32_t> code) {
    if (pc % 4) return 0;
    for (std::size_t i = 0; i + 2 < code.size(); ++i) {
        if ((code[i] & 0x9f00001fu) == 0x90000008u &&
            (code[i+1] & 0xffc003ffu) == 0xf9400108u &&
            (code[i+2] & 0xff00001fu) == 0xb4000008u)
            return adrp_target(pc + i * 4, code[i]) + (((code[i+1] >> 10) & 4095u) * 8);
    }
    return 0;
}
}
