// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gfx/sdk_graphics_decode.h"
#include <array>
#include <cstdio>

// Decode only the qualified SDK's initialization slots. These checks keep
// an unfamiliar SDK instruction sequence from turning into a guest write.
int main() {
    using namespace nxrt::ps5_graphics;
    unsigned checks = 0, failures = 0;
    auto expect = [&](bool result, const char* name) {
        ++checks;
        if (!result) { ++failures; std::printf("FAIL %s\n", name); }
    };
    constexpr uint64_t pc = 0x75003f84d0ull;
    const std::array<uint32_t, 4> flag{0xd0005508, 0x911fa108, 0xb8bfc108, 0x7100051f};
    expect(initialized_flag(pc, flag) == 0x7500e9a7e8ull, "qualified initialized flag");
    expect(adrp_target(0x800001234ull, 0xf0ffffe8) == 0x800000000ull, "negative page displacement");
    expect(adrp_target(0x800001ffCull, 0xb0000008) == 0x800002000ull, "positive page displacement");
    expect(adrp_target(0x800001234ull, 0x90000008) == 0x800001000ull, "zero page displacement");
    expect(initialized_flag(pc + 1, flag) == 0, "unaligned code");
    for (std::size_t n = 0; n < flag.size(); ++n)
        expect(initialized_flag(pc, std::span(flag).first(n)) == 0, "truncated flag sequence");
    for (std::size_t n = 0; n < flag.size(); ++n) {
        auto wrong = flag; wrong[n] ^= 1;
        expect(initialized_flag(pc, wrong) == 0, "wrong register or immediate");
    }
    auto shifted = flag; shifted[1] |= 1u << 22;
    expect(initialized_flag(pc, shifted) == 0, "unsupported shifted ADD");
    const std::array<uint32_t, 3> slot{0xd0005228, 0xf942c108, 0xb4000048};
    expect(driver_pointer(0x7500575fecull, slot) == 0x7500fbb580ull, "qualified driver pointer");
    std::array<uint32_t, 18> padded{};
    for (std::size_t n = 0; n < slot.size(); ++n) padded[n+15] = slot[n];
    expect(driver_pointer(0x7500575fb0ull, padded) == 0x7500fbb580ull, "driver sequence after prologue");
    for (std::size_t n = 0; n < slot.size(); ++n) {
        expect(driver_pointer(pc, std::span(slot).first(n)) == 0, "truncated driver sequence");
        auto wrong = slot; wrong[n] ^= 1;
        expect(driver_pointer(pc, wrong) == 0, "wrong driver register");
    }
    expect(driver_pointer(pc+1, slot) == 0, "unaligned driver code");
    std::printf("SDK graphics decoder: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
