// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "../gfx/offline_guest_displays.h"
#include <array>
#include <limits>

using nxrt::ps5_graphics::OfflineGuestDisplays;
NXTEST_GROUP(offline_guest_displays)

NXTEST(native_window_lifetime_without_supervisor) {
    alignas(16) std::array<unsigned char, 256> primary_window{};
    const auto window = reinterpret_cast<uint64_t>(primary_window.data());
    OfflineGuestDisplays displays;
    CHECK(!displays.active()); CHECK(displays.window(0) == 0);
    REQUIRE(displays.create(1, window));
    CHECK(displays.active()); CHECK(displays.window(0) == window);
    CHECK(displays.window(1) == 0); CHECK(displays.window(UINT32_MAX) == 0);
    // Duplicate creation must preserve the active window and guest count.
    CHECK(!displays.create(3, window + 16));
    CHECK(displays.window(0) == window); CHECK(displays.window(1) == 0);
    displays.destroy();
    CHECK(!displays.active()); CHECK(displays.window(0) == 0);
    displays.destroy();
    REQUIRE(displays.create(3, window));
    for (uint32_t id = 0; id < 3; ++id) CHECK(displays.window(id) == window);
    CHECK(displays.window(3) == 0);
    // The borrowed native object remains owned and writable by its display.
    displays.destroy(); primary_window[0] = 42;
    CHECK(primary_window[0] == 42);
}

NXTEST(invalid_setup_preserves_state) {
    OfflineGuestDisplays displays;
    for (const int32_t count : {std::numeric_limits<int32_t>::min(), -1, 0, 4,
                               std::numeric_limits<int32_t>::max()}) {
        CHECK(!displays.create(count, 0x1000));
        CHECK(!displays.active()); CHECK(displays.window(0) == 0);
    }
    CHECK(!displays.create(1, 0));
    CHECK(!displays.active());
    REQUIRE(displays.create(2, 0x2000));
    CHECK(displays.window(1) == 0x2000); CHECK(displays.window(2) == 0);
    displays.destroy();
    REQUIRE(displays.create(1, 0x3000));
    CHECK(displays.window(0) == 0x3000); CHECK(displays.window(1) == 0);
}
