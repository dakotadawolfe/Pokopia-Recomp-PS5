// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

NXTEST_GROUP(app_runtime)
namespace {
std::atomic<unsigned> destroyed{0};
struct ThreadValue { ~ThreadValue() { ++destroyed; } unsigned value = 0; };
thread_local ThreadValue local_value;
}
NXTEST(native_app_runs_thread_local_destructors) {
    destroyed = 0;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([i] { local_value.value = i + 1; });
    for (auto& thread : threads) thread.join();
    CHECK_EQ(destroyed.load(), 8u);
}
NXTEST(native_app_unwinds_cpp_objects) {
    unsigned cleanups = 0;
    struct Cleanup { unsigned& count; ~Cleanup() { ++count; } };
    bool caught = false;
    try {
        Cleanup cleanup{cleanups};
        throw std::runtime_error("native runtime exception probe");
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "native runtime exception probe";
    }
    CHECK(caught);
    CHECK_EQ(cleanups, 1u);
}
