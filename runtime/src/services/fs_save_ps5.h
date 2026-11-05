// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nxrt/ipc.h"
#include <atomic>
namespace nxrt::services::ps5save {
// Development backend in title-owned download storage. Native SaveData UI
// integration is separate; this does not modify the system save database.
constexpr int64_t maximum_size = 96ll << 20;
void add_commands(ipc::Service&, std::shared_ptr<std::atomic<bool>> bound);
void reset();
uint32_t extend(uint64_t uid_hi, uint64_t uid_lo, int64_t size, int64_t journal);
}
