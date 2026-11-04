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
struct StartupState {
    // Service wire values: 0 = normal, 1 = legacy fast load, 2 = this title's
    // SDK fast load. The native clock policy remains fixed on PS5.
    uint32_t cpu_boost_mode = 0;
    bool notified_running = false;
};
StartupState startup_state();
// Publish the renderer's actual output size. Invalid sizes leave state intact.
// Establishing the initial size does not signal a change; later changes do.
bool set_display_resolution(int32_t width, int32_t height);
// Account a completed suspension measured in kernel::tick_hz() units. This does
// not suspend workers. Returns false on overflow without changing state/events.
bool record_suspended_ticks(uint64_t elapsed);
// The renderer adapter can signal the guest's GPU-failure event. There is no
// host GPU connection yet; this is the notification bridge, not error detection.
void signal_gpu_error();
} // namespace nxrt::services::amctl
