// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace nxrt::services::amctl {
// Requested guest policy. The PS5 shell/scheduler adapter still has to apply it.
struct LifecyclePolicy {
    std::array<bool, 3> focus_handling{};
    bool restart_message = false;
    bool out_of_focus_suspending = true;
    bool exit_library_on_next_program = false;
};
LifecyclePolicy lifecycle_policy();
// Account a completed suspension measured in kernel::tick_hz() units. This does
// not suspend workers. Returns false on overflow without changing state/events.
bool record_suspended_ticks(uint64_t elapsed);
// The renderer adapter can signal the guest's GPU-failure event. There is no
// host GPU connection yet; this is the notification bridge, not error detection.
void signal_gpu_error();
} // namespace nxrt::services::amctl
