// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/svc.h"
#include <limits>

namespace nxrt {
uint64_t svc_total_memory() {
    const int64_t mb = config_get_int("memory", "app_memory_mb", 0);
    if (mb > 0) {
        const auto maximum = std::numeric_limits<uint64_t>::max();
        return static_cast<uint64_t>(mb) > (maximum >> 20) ? maximum : static_cast<uint64_t>(mb) << 20;
    }
    // The guest application's resource limit, not the host's free memory.
    // Preserve upstream's Switch 2 (10.5 GiB) and Switch 1 pool sizes.
    return game::info().platform == game::Platform::Switch2 ? 0x2A0000000ull : 0xCD500000ull;
}
} // namespace nxrt
