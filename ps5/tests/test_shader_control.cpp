// SPDX-License-Identifier: GPL-3.0-or-later
#include "nvn_shaders.h"
#include <array>
#include <cstdio>
#include <cstring>
int main() {
    std::array<uint8_t, 0xd8> bytes{};
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put(0, nxrt::nvn::be::kShaderControlMagic); put(4, 201);
    put(0x38, 0x180); put(0x3c, 16); put(0x40, 0x180); put(0x44, 0x200);
    put(0x50, 0x140); bytes[0x56] = 5;
    put(0x58, 8); put(0x5c, 4); put(0x60, 2); put(0x64, 0x1800);
    const auto control = nxrt::nvn::be::read_control(bytes.data());
    if (!control.valid || !control.nvn2 || control.shared_size != 0x1800 || control.stage != 5 ||
        control.local_size[0] != 8 || control.local_size[1] != 4 || control.local_size[2] != 2 || control.code_size != 0x200) return 1;
    put(0x64, 0);
    if (nxrt::nvn::be::read_control(bytes.data()).shared_size != 0) return 2;
    std::puts("NVN2 local and shared memory fields remain distinct");
}
