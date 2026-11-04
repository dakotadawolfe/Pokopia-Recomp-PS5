// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/kernel.h"
#include "nxrt/game.h"
#include <chrono>
#include <thread>

namespace nxrt::kernel {
uint64_t tick_hz() {
    return game::info().platform == game::Platform::Switch2 ? kTickHzSwitch2 : kTickHzSwitch1;
}
uint64_t system_tick() {
    using Clock = std::chrono::steady_clock;
    static const auto origin = Clock::now();
    const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin).count();
    const uint64_t hz = tick_hz();
    return ns / 1000000000 * hz + (ns % 1000000000) * hz / 1000000000;
}
void yield_host() { std::this_thread::yield(); }
void sleep_ns(int64_t ns) {
    if (ns <= 0) yield_host();
    else std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
}
