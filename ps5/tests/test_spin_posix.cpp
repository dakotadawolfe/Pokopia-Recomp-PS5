// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "test_framework.h"
#include <atomic>
#include <chrono>
#include <thread>

NXTEST_GROUP(spin_posix)
namespace {
constexpr auto lock_name = "_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE";
struct Mutex {
    uint8_t prefix[16]{};
    std::atomic<uint32_t> word{0};
};
static_assert(offsetof(Mutex, word) == 16);
struct Tls {
    uint8_t prefix[256]{};
    uint16_t disabled = 0;
};
void acquire(Mutex& mutex, Tls& tls) {
    Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(&mutex);
    c.tpidrro = reinterpret_cast<uint64_t>(&tls);
    nxrt::hle_lookup(lock_name)(&c);
}
void install() {
    nxrt::config_set("sched", "busy_mutex_hle", "1");
    nxrt::config_set("sched", "spin_yield", "1");
    nxrt::kernel::install_busy_mutex_hle();
    nxrt::kernel::install_spin_hint();
}
}

NXTEST(busy_mutex_preserves_nested_disable_count) {
    install();
    REQUIRE(nxrt::hle_lookup(lock_name));
    for (uint16_t previous : {uint16_t(0), uint16_t(2)}) {
        Mutex mutex;
        Tls tls;
        tls.disabled = previous;
        acquire(mutex, tls);
        CHECK_EQ(mutex.word.load(), 1u);
        CHECK_EQ(tls.disabled, previous + 1);
    }
}

NXTEST(contended_busy_mutex_observes_guest_release) {
    install();
    Mutex mutex;
    Tls tls;
    mutex.word.store(1);
    int protected_value = 0;
    int observed = 0;
    std::atomic<bool> entered{false};
    std::thread waiter([&] { entered = true; acquire(mutex, tls); observed = protected_value; });
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    protected_value = 42;
    mutex.word.store(0, std::memory_order_release);
    waiter.join();
    CHECK_EQ(mutex.word.load(), 1u);
    CHECK_EQ(tls.disabled, 1);
    CHECK_EQ(observed, 42);
}

NXTEST(busy_mutex_serializes_shared_updates) {
    install();
    Mutex mutex;
    unsigned shared = 0;
    std::thread workers[4];
    for (auto& worker : workers) worker = std::thread([&] {
        Tls tls;
        for (unsigned n = 0; n < 2000; ++n) {
            acquire(mutex, tls);
            ++shared;
            --tls.disabled;
            mutex.word.store(0, std::memory_order_release);
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK_EQ(shared, 8000u);
    CHECK_EQ(mutex.word.load(), 0u);
}

NXTEST(wfe_yield_hook_allows_a_waiting_worker_to_finish) {
    install();
    REQUIRE(nx_spin_hook);
    std::atomic<bool> ready{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ready.store(true, std::memory_order_release);
    });
    while (!ready.load(std::memory_order_acquire)) nx_spin_hint(1);
    producer.join();
    nx_spin_hint(0);
    CHECK(ready.load());
    nx_spin_hook = nullptr;
}
