// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>
#include <pthread.h>

NXTEST_GROUP(app_runtime)
namespace {
std::atomic<unsigned> destroyed{0};
struct ThreadValue { ~ThreadValue() { ++destroyed; } unsigned value = 0; };
thread_local ThreadValue local_value;
std::atomic<unsigned> order{0};
struct OrderedValue { unsigned value; ~OrderedValue() { order = order.load() * 10 + value; } };
thread_local OrderedValue first{1}, second{2};
}
NXTEST(native_app_runs_thread_local_destructors) {
    destroyed = 0;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([i] { local_value.value = i + 1; });
    for (auto& thread : threads) thread.join();
    CHECK_EQ(destroyed.load(), 8u);
}
NXTEST(native_app_thread_destructors_run_in_reverse_order) {
    order = 0;
    std::thread thread([] { first.value = 1; second.value = 2; });
    thread.join();
    CHECK_EQ(order.load(), 21u);
}
NXTEST(native_app_pthread_exit_keeps_tls_alive_for_destructors) {
    order = 0;
    pthread_t thread;
    REQUIRE(pthread_create(&thread, nullptr, +[](void*) -> void* {
        first.value = 3; second.value = 4;
        pthread_exit(reinterpret_cast<void*>(uintptr_t{0x1234}));
    }, nullptr) == 0);
    void* result = nullptr;
    REQUIRE(pthread_join(thread, &result) == 0);
    CHECK_EQ(reinterpret_cast<uintptr_t>(result), uintptr_t{0x1234});
    CHECK_EQ(order.load(), 43u);
}
NXTEST(native_app_cancel_keeps_tls_alive_for_destructors) {
    order = 0;
    std::atomic<bool> ready{false};
    pthread_t thread;
    REQUIRE(pthread_create(&thread, nullptr, +[](void* argument) -> void* {
        first.value = 5; second.value = 6;
        static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
        for (;;) { pthread_testcancel(); std::this_thread::yield(); }
    }, &ready) == 0);
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    const int cancel_result = pthread_cancel(thread);
    CHECK_EQ(cancel_result, 0);
    void* result = nullptr;
    REQUIRE(pthread_join(thread, &result) == 0);
    CHECK(result == PTHREAD_CANCELED);
    CHECK_EQ(order.load(), 65u);
}
extern "C" int __cxa_thread_atexit_impl(void (*)(void*), void*, void*);
NXTEST(native_app_destructor_can_register_another_destructor) {
    std::atomic<int> status{0};
    std::thread thread([&] {
        auto late = +[](void* value) { *static_cast<std::atomic<int>*>(value) += 10; };
        struct Register {
            static void run(void* value) {
                auto next = +[](void* p) { *static_cast<std::atomic<int>*>(p) += 100; };
                *static_cast<std::atomic<int>*>(value) += 1;
                if (__cxa_thread_atexit_impl(next, value, nullptr) != 0) *static_cast<std::atomic<int>*>(value) = -1000;
            }
        };
        if (__cxa_thread_atexit_impl(late, &status, nullptr) != 0 ||
            __cxa_thread_atexit_impl(Register::run, &status, nullptr) != 0) status = -1000;
    });
    thread.join();
    CHECK_EQ(status.load(), 111);
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
