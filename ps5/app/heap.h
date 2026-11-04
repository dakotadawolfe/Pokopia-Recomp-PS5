// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace nxrt::app_heap {
struct Stats {
    size_t spaces;
    size_t backing_bytes;
    size_t live_blocks;
    size_t live_bytes;
    unsigned failed_operation;
    int platform_error;
    size_t failed_bytes;
};
// Concurrent snapshots are approximate; spaces remain mapped until app exit.
Stats stats() noexcept;
bool owns(const void* address) noexcept;
}
