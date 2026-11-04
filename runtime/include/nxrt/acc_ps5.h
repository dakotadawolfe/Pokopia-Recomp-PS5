// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace nxrt::services {
void register_acc();
void acc_reset();
namespace accapi {
// Available only while this process has a registered offline account snapshot.
bool launch_user(uint64_t* hi, uint64_t* lo);
}
}
