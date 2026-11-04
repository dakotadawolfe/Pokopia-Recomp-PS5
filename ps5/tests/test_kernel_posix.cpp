// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrecomp/ctx.h"
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <thread>

NXTEST_GROUP(kernel_posix)
namespace ks = nxrt::kernel;
namespace mem = nxrt::mem;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t entry = 0x7100010000;
constexpr int64_t ms = 1000000;
extern "C" const NxModule nxmod_subpage_fixture;

[[noreturn]] void watchdog(const char* message) {
    std::fprintf(stderr, "KERNEL TEST WATCHDOG: %s\n", message);
    std::fflush(stderr);
    std::abort();
}
template<class F> void until(F predicate) {
    const auto end = Clock::now() + 5s;
    while (!predicate()) {
        if (Clock::now() >= end) watchdog("condition did not become ready");
        std::this_thread::sleep_for(1ms);
    }
}
void join(uint32_t h) { if (!ks::thread_join(h, 5000)) watchdog("guest thread did not terminate"); }
int64_t queued(const char* name) {
    for (const auto& [key, value] : nxrt::gauge_read_all()) if (key == name) return value;
    return -2;
}
uint64_t address(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint32_t load(const uint32_t& word) { return __atomic_load_n(&word, __ATOMIC_SEQ_CST); }
bool cas(uint32_t& word, uint32_t expected, uint32_t next) {
    return __atomic_compare_exchange_n(&word, &expected, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    Fixture() {
        ks::kernel_reset();
        nxrt::modules_unload_all(); nxrt::fn_clear(); nxrt::hle_clear(); mem::shutdown();
        nxrt::game::set_current(&game);
        if (!mem::init({.heap_reserve = 64ull << 20})) watchdog("memory init failed");
        nxrt::fn_register(entry, +[](::Ctx* c) {
            (*reinterpret_cast<std::function<void(::Ctx*)>*>(c->x[0]))(c);
        });
    }
    ~Fixture() {
        ks::main_thread_release();
        ks::kernel_reset();
        nxrt::modules_unload_all(); nxrt::fn_clear(); nxrt::hle_clear(); mem::shutdown();
        nxrt::game::set_current(previous);
    }
};
struct Job {
    std::function<void(::Ctx*)> function;
    uint32_t handle;
    Job(std::function<void(::Ctx*)> f, int priority = 44) : function(std::move(f)) {
        ks::ThreadParams p;
        p.entry = entry; p.arg = address(&function); p.stack_top = mem::kStackBase + 0x10000;
        p.prio = priority; p.host_stack_bytes = 2 << 20;
        handle = ks::thread_create(p);
        if (!handle) watchdog("guest create failed");
    }
    void start() { if (ks::thread_start(handle) != ks::kOk) watchdog("guest start failed"); }
    void finish() { join(handle); }
};
struct AtomicEvent : ks::KObject {
    std::atomic<bool> ready{false};
    bool signaled() const override { return ready.load(); }
    ks::ObjType type() const override { return ks::ObjType::Event; }
    void signal() { ready = true; ks::notify_waitable_changed(); }
};
std::atomic<unsigned> tls_drops{0};
struct Local { unsigned value = 0; ~Local() { tls_drops += value; } };
thread_local Local local;

// The guest SDK fast path uses atomics, and calls the kernel only on contention.
struct GuestMutex {
    uint32_t word = 0;
    void lock() {
        const uint32_t tag = ks::current_thread()->handle;
        for (;;) {
            auto cur = load(word);
            if (!cur) { if (cas(word, 0, tag)) return; continue; }
            if ((cur & ~ks::kHandleWaitMask) == tag) return;
            if (!(cur & ks::kHandleWaitMask)) {
                if (!cas(word, cur, cur | ks::kHandleWaitMask)) continue;
                cur |= ks::kHandleWaitMask;
            }
            if (ks::arbitrate_lock(cur & ~ks::kHandleWaitMask, address(&word), tag)) watchdog("mutex lock failed");
        }
    }
    void unlock() {
        const uint32_t tag = ks::current_thread()->handle;
        if (!cas(word, tag, 0) && ks::arbitrate_unlock(address(&word))) watchdog("mutex unlock failed");
    }
};
struct GuestCond {
    uint32_t key = 0;
    void wait(GuestMutex& mutex) {
        if (ks::wait_process_wide_key(address(&mutex.word), address(&key), ks::current_thread()->handle, -1))
            watchdog("condition wait failed");
    }
    void signal() {
        if (load(key) && ks::signal_process_wide_key(address(&key), -1)) watchdog("condition signal failed");
    }
};
}

NXTEST(guest_create_initializes_registers_and_independent_tls) {
    Fixture fixture;
    Job first([](::Ctx*) {}), second([](::Ctx*) {});
    auto a = ks::handle_as<ks::KThread>(first.handle), b = ks::handle_as<ks::KThread>(second.handle);
    REQUIRE(a && b);
    CHECK(a->id != b->id && a->tls != b->tls && a->tp_block != b->tp_block);
    CHECK_EQ(a->ctx.x[0], address(&first.function));
    CHECK_EQ(a->ctx.x[31], mem::kStackBase + 0x10000);
    CHECK_EQ(a->ctx.ex_addr, UINT64_MAX);
    CHECK_EQ(a->ctx.tpidr, a->tp_block);
    CHECK_EQ(a->ctx.tpidrro, a->tls);
    CHECK_EQ(*reinterpret_cast<uint64_t*>(a->tls + 0x1f0), a->tp_block);
    CHECK_EQ(*reinterpret_cast<uint64_t*>(a->tp_block), 0u);
    CHECK(!a->started && !a->worker && !a->exited);
    CHECK_EQ(ks::thread_count_running(), 0u);
}
NXTEST(guest_tls_copies_the_module_template_and_zeroes_bss) {
    Fixture fixture;
    std::string error;
    auto* module = nxrt::module_load_file(NXRT_TEST_DATA_DIR "/fixture_aarch64.elf", {}, &error);
    REQUIRE(module);
    REQUIRE(nxrt::module_link_all()); // The process TLS template is published during linking.
    const auto tpl = nxrt::module_tls_template();
    REQUIRE(tpl.file_size && tpl.mem_size > tpl.file_size);
    Job first([](::Ctx*) {}), second([](::Ctx*) {});
    auto a = ks::handle_as<ks::KThread>(first.handle), b = ks::handle_as<ks::KThread>(second.handle);
    auto* data = reinterpret_cast<unsigned char*>(a->tp_block + tpl.tp_offset);
    CHECK_EQ(std::memcmp(data, reinterpret_cast<void*>(tpl.init_addr), tpl.file_size), 0);
    for (auto i = tpl.file_size; i < tpl.mem_size; ++i) CHECK_EQ(data[i], 0u);
    data[0] ^= 0xff;
    CHECK(data[0] != *reinterpret_cast<unsigned char*>(b->tp_block + tpl.tp_offset));
}
NXTEST(guest_start_dispatches_once_and_join_includes_tls_cleanup) {
    Fixture fixture;
    tls_drops = 0;
    uint32_t pseudo = 0;
    bool own_ctx = false;
    Job job([&](::Ctx* c) {
        local.value = 13;
        own_ctx = ks::current_ctx() == c;
        pseudo = ks::handle_as<ks::KThread>(ks::kPseudoCurrentThread)->handle;
        c->x[5] = 0xabcdef;
    });
    job.start(); job.finish();
    CHECK_EQ(ks::thread_start(job.handle), ks::kInvalidState);
    CHECK_EQ(tls_drops.load(), 13u);
    CHECK(own_ctx);
    CHECK_EQ(pseudo, job.handle);
    auto t = ks::handle_as<ks::KThread>(job.handle);
    CHECK(t->exited && t->signaled());
    CHECK_EQ(t->ctx.x[5], 0xabcdefu);
    CHECK_EQ(t->worker->stack_size(), 2u << 20);
    CHECK(!ks::current_thread());
}
NXTEST(guest_rejects_self_join_and_repeat_start_during_external_join) {
    Fixture fixture;
    bool rejected = false;
    Job job([&](::Ctx*) {
        auto h = ks::current_thread()->handle;
        rejected = !ks::thread_join(h, 0) && ks::thread_start(h) == ks::kInvalidState;
    });
    job.start(); job.finish();
    CHECK(rejected);
}
NXTEST(guest_explicit_exit_marks_waitable_and_runs_tls_destructors) {
    Fixture fixture;
    tls_drops = 0;
    int stage = 0;
    Job job([&](::Ctx*) { local.value = 7; stage = 1; ks::thread_exit(); stage = 2; });
    job.start();
    CHECK_EQ(ks::wait_synchronization(&job.handle, 1, 5000 * ms, nullptr), ks::kOk);
    job.finish();
    CHECK_EQ(stage, 1);
    CHECK_EQ(tls_drops.load(), 7u);
    CHECK(!ks::handle_as<ks::KThread>(job.handle)->worker->failure());
}
NXTEST(guest_exception_terminates_worker_and_retains_the_error) {
    Fixture fixture;
    Job job([](::Ctx*) { throw std::runtime_error("guest test error"); });
    job.start(); job.finish();
    auto t = ks::handle_as<ks::KThread>(job.handle);
    CHECK(t->exited);
    REQUIRE(t->worker->failure());
    bool correct = false;
    try { std::rethrow_exception(t->worker->failure()); }
    catch (const std::runtime_error& e) { correct = std::string(e.what()) == "guest test error"; }
    CHECK(correct);
}
NXTEST(closing_a_live_guest_handle_keeps_its_worker_and_context_owned) {
    Fixture fixture;
    std::atomic<bool> release{false}, entered{false};
    Job job([&](::Ctx*) { entered = true; while (!release) std::this_thread::yield(); });
    auto t = ks::handle_as<ks::KThread>(job.handle);
    job.start(); until([&] { return entered.load(); });
    CHECK(!ks::thread_join(job.handle, 2));
    ks::handle_close(job.handle);
    CHECK(!ks::handle_get(job.handle));
    CHECK_EQ(ks::thread_count_running(), 1u);
    release = true;
    if (!t->worker->wait_for(5s) || !t->worker->join()) watchdog("closed-handle worker did not exit");
    CHECK(t->exited);
}
NXTEST(completed_guest_workers_are_reaped_outside_the_list_lock) {
    Fixture fixture;
    for (int i = 0; i < 40; ++i) {
        Job job([](::Ctx*) {});
        job.start(); job.finish(); ks::handle_close(job.handle);
    }
    Job last([](::Ctx*) {});
    CHECK_EQ(ks::threads().size(), 1u);
    CHECK_EQ(ks::handle_count(), 1u);
}
NXTEST(adopted_main_has_a_real_guest_stack_and_pseudo_handle) {
    Fixture fixture;
    auto handle = ks::main_thread_adopt(0x10000);
    REQUIRE(handle);
    auto t = ks::handle_as<ks::KThread>(handle);
    CHECK_EQ(ks::handle_get(ks::kPseudoCurrentThread).get(), t.get());
    CHECK_EQ(mem::vm_query(t->stack_top - 1).state, mem::kMemStack);
    CHECK(mem::is_mapped(t->stack_top - 0x10000, 0x10000));
    CHECK_EQ(ks::main_thread_adopt(0x10000), 0u);
    CHECK(!ks::thread_join(handle, 0));
    bool rejected = false;
    try { ks::kernel_reset(); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    ks::main_thread_release();
    CHECK(t->exited && !ks::current_thread());
    CHECK(ks::thread_join(handle, 0));
}
NXTEST(guest_priority_core_mask_and_failed_start_validation) {
    Fixture fixture;
    ks::ThreadParams p;
    p.prio = 64; CHECK_EQ(ks::thread_create(p), 0u);
    p.prio = 44; p.entry = entry; p.host_stack_bytes = 1;
    auto h = ks::thread_create(p);
    REQUIRE(h);
    CHECK_EQ(ks::thread_start(h), ks::kOutOfResource);
    auto t = ks::handle_as<ks::KThread>(h);
    CHECK(!t->started && !t->worker);
    CHECK(!ks::thread_set_priority(*t, -1));
    CHECK(ks::thread_set_priority(*t, 28));
    CHECK_EQ(t->prio, 28);
    CHECK_EQ(ks::app_core_count(), 6);
    CHECK(ks::thread_set_core_mask(*t, 5, 0x30));
    CHECK_EQ(t->ideal_core, 5);
    CHECK_EQ(t->core_mask, 0x30u);
    CHECK(!ks::thread_set_core_mask(*t, 0, 0x80));
    CHECK_EQ(ks::thread_start(0x1234), ks::kInvalidHandle);
}
NXTEST(handle_table_limits_and_destroys_objects_outside_its_lock) {
    Fixture fixture;
    auto object = std::make_shared<AtomicEvent>();
    CHECK_EQ(ks::handle_add(nullptr), 0u);
    for (int i = 0; i < 0x3000; ++i) REQUIRE(ks::handle_add(object));
    CHECK_EQ(ks::handle_add(object), 0u);
    CHECK_EQ(ks::handle_count(), 0x3000u);
    ks::handles_reset();
    struct InspectOnDrop : ks::KObject { ~InspectOnDrop() { (void)ks::handle_count(); } };
    const auto h = ks::handle_add(std::make_shared<InspectOnDrop>());
    ks::handle_close(h);
    CHECK_EQ(ks::handle_count(), 0u);
}
NXTEST(guest_clock_matches_each_platform_and_sleep_is_monotonic) {
    Fixture fixture;
    for (auto platform : {nxrt::game::Platform::Switch1, nxrt::game::Platform::Switch2}) {
        fixture.game.platform = platform;
        CHECK_EQ(ks::tick_hz(), platform == nxrt::game::Platform::Switch1 ? 19200000u : 31250000u);
        const auto start = Clock::now();
        const auto tick = ks::system_tick();
        ks::sleep_ns(20 * ms);
        const double seconds = double(ks::system_tick() - tick) / ks::tick_hz();
        const double wall = std::chrono::duration<double>(Clock::now() - start).count();
        CHECK(seconds >= 0.019);
        CHECK(seconds > wall - 0.002 && seconds < wall + 0.002);
        ks::sleep_ns(0); ks::sleep_ns(-1);
    }
}
NXTEST(guest_mutex_handoff_uses_priority_then_fifo_and_preserves_wait_bit) {
    Fixture fixture;
    uint32_t word = 0x12345 | ks::kHandleWaitMask;
    std::array<int, 3> order{};
    std::atomic<unsigned> next{0};
    std::array<std::unique_ptr<Job>, 3> jobs;
    for (int i = 0; i < 3; ++i) {
        jobs[i] = std::make_unique<Job>([&, i](::Ctx*) {
            if (ks::arbitrate_lock(0x12345, address(&word), ks::current_thread()->handle)) watchdog("arbitrate failed");
            order[next++] = i;
            if (ks::arbitrate_unlock(address(&word))) watchdog("unlock failed");
        }, i == 0 ? 44 : 28);
        jobs[i]->start(); until([&] { return queued("kernel.waiters_mutex") == i + 1; });
    }
    CHECK_EQ(load(word), 0x12345u | ks::kHandleWaitMask);
    CHECK_EQ(ks::arbitrate_unlock(address(&word)), ks::kOk);
    for (auto& job : jobs) job->finish();
    CHECK_EQ(order[0], 1); CHECK_EQ(order[1], 2); CHECK_EQ(order[2], 0);
    CHECK_EQ(load(word), 0u);
    CHECK_EQ(queued("kernel.waiters_mutex"), 0);
}
NXTEST(guest_condition_publishes_waiters_before_releasing_the_mutex) {
    Fixture fixture;
    GuestMutex mutex;
    GuestCond condition;
    unsigned received = 0;
    bool ready = false, acknowledged = false;
    Job consumer([&](::Ctx*) {
        for (int i = 0; i < 1000; ++i) {
            mutex.lock();
            while (!ready) condition.wait(mutex);
            ready = false; acknowledged = true; ++received;
            condition.signal(); mutex.unlock();
        }
    });
    Job producer([&](::Ctx*) {
        for (int i = 0; i < 1000; ++i) {
            mutex.lock(); ready = true; condition.signal();
            while (!acknowledged) condition.wait(mutex);
            acknowledged = false; mutex.unlock();
        }
    });
    consumer.start(); producer.start(); consumer.finish(); producer.finish();
    CHECK_EQ(received, 1000u);
    CHECK_EQ(load(condition.key), 0u);
    CHECK_EQ(load(mutex.word), 0u);
}
NXTEST(condition_timeout_removes_a_waiter_transferred_to_a_busy_mutex) {
    Fixture fixture;
    uint32_t mutex = 0x12345, key = 0;
    uint32_t result = 0;
    Job job([&](::Ctx*) {
        result = ks::wait_process_wide_key(address(&mutex), address(&key), ks::current_thread()->handle, 100 * ms);
    });
    job.start(); until([&] { return queued("kernel.waiters_cv") == 1; });
    if (!cas(mutex, 0, 0x54321)) watchdog("wait did not release the condition mutex");
    CHECK_EQ(ks::signal_process_wide_key(address(&key), 1), ks::kOk);
    until([&] { return queued("kernel.waiters_mutex") == 1; });
    job.finish();
    CHECK_EQ(result, ks::kTimedOut);
    CHECK_EQ(queued("kernel.waiters_cv"), 0);
    CHECK_EQ(queued("kernel.waiters_mutex"), 0);
    CHECK_EQ(load(key), 0u);
    CHECK_EQ(ks::arbitrate_unlock(address(&mutex)), ks::kOk);
    CHECK_EQ(load(mutex), 0u);
}
NXTEST(address_conditions_decrement_and_wrapping_signal_arithmetic) {
    Fixture fixture;
    uint32_t word = 4;
    CHECK_EQ(ks::wait_for_address(address(&word) + 1, 0, 5, 0), ks::kInvalidAddress);
    CHECK_EQ(ks::wait_for_address(address(&word), 3, 5, 0), ks::kInvalidEnum);
    CHECK_EQ(ks::wait_for_address(address(&word), 2, 5, 0), ks::kInvalidState);
    CHECK_EQ(ks::wait_for_address(address(&word), 1, 5, 0), ks::kTimedOut);
    CHECK_EQ(word, 3u);
    word = INT_MAX;
    CHECK_EQ(ks::signal_to_address(address(&word), 1, INT_MAX, 1), ks::kOk);
    CHECK_EQ(word, uint32_t(INT_MIN));
    word = INT_MAX;
    CHECK_EQ(ks::signal_to_address(address(&word), 2, INT_MAX, 1), ks::kOk);
    CHECK_EQ(word, uint32_t(INT_MIN));
}
NXTEST(address_signal_wakes_the_requested_count_and_broadcasts_the_rest) {
    Fixture fixture;
    uint32_t word = 0;
    std::atomic<unsigned> woken{0};
    std::array<std::unique_ptr<Job>, 3> jobs;
    for (auto& job : jobs) {
        job = std::make_unique<Job>([&](::Ctx*) {
            if (ks::wait_for_address(address(&word), 2, 0, 5000 * ms) == ks::kOk) ++woken;
        });
        job->start();
    }
    until([&] { return queued("kernel.waiters_addr") == 3; });
    CHECK_EQ(ks::signal_to_address(address(&word), 1, 0, 1), ks::kOk);
    until([&] { return woken.load() == 1; });
    CHECK_EQ(queued("kernel.waiters_addr"), 2);
    CHECK_EQ(load(word), 1u);
    CHECK_EQ(ks::signal_to_address(address(&word), 0, 0, -1), ks::kOk);
    for (auto& job : jobs) job->finish();
    CHECK_EQ(woken.load(), 3u);
}
NXTEST(wait_synchronization_keeps_closed_objects_alive_and_reports_index) {
    Fixture fixture;
    auto first = std::make_shared<AtomicEvent>(), second = std::make_shared<AtomicEvent>();
    uint32_t handles[] = {ks::handle_add(first), ks::handle_add(second)};
    int32_t index = -1;
    uint32_t result = 0;
    Job job([&](::Ctx*) { result = ks::wait_synchronization(handles, 2, 5000 * ms, &index); });
    job.start(); until([&] { return queued("kernel.waiters_sync") == 1; });
    ks::handle_close(handles[1]); second->signal(); job.finish();
    CHECK_EQ(result, ks::kOk); CHECK_EQ(index, 1);
    CHECK_EQ(ks::wait_synchronization(handles, 2, 0, &index), ks::kInvalidHandle);
    CHECK_EQ(index, -1);
    CHECK_EQ(ks::wait_synchronization(nullptr, 65, 0, nullptr), ks::kOutOfRange);
}
NXTEST(cancellation_is_remembered_and_wakes_a_current_sync_wait) {
    Fixture fixture;
    std::array<uint32_t, 3> results{};
    Job job([&](::Ctx*) {
        results[0] = ks::wait_synchronization(nullptr, 0, -1, nullptr);
        results[1] = ks::wait_synchronization(nullptr, 0, 0, nullptr);
        results[2] = ks::wait_synchronization(nullptr, 0, -1, nullptr);
    });
    CHECK_EQ(ks::cancel_synchronization(job.handle), ks::kOk);
    job.start(); until([&] { return queued("kernel.waiters_sync") == 1; });
    CHECK_EQ(ks::cancel_synchronization(job.handle), ks::kOk);
    job.finish();
    CHECK_EQ(results[0], ks::kCancelled);
    CHECK_EQ(results[1], ks::kTimedOut);
    CHECK_EQ(results[2], ks::kCancelled);
}
NXTEST(sync_deadlines_use_the_platform_clock_even_with_repeated_wakes) {
    Fixture fixture;
    for (auto platform : {nxrt::game::Platform::Switch1, nxrt::game::Platform::Switch2}) {
        fixture.game.platform = platform;
        std::atomic<bool> done{false};
        double elapsed = 0;
        uint32_t result = 0;
        Job job([&](::Ctx*) {
            const auto start = Clock::now();
            result = ks::wait_synchronization(nullptr, 0, 80 * ms, nullptr);
            elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            done = true;
        });
        job.start();
        while (!done) { ks::notify_waitable_changed(); std::this_thread::sleep_for(1ms); }
        job.finish();
        CHECK_EQ(result, ks::kTimedOut);
        CHECK(elapsed >= 79.0 && elapsed < 2000.0);
    }
}
NXTEST(maximum_finite_timeouts_do_not_overflow_into_immediate_expiry) {
    Fixture fixture;
    uint32_t word = 0, result = 0;
    Job job([&](::Ctx*) { result = ks::wait_for_address(address(&word), 2, 0, INT64_MAX); });
    job.start(); until([&] { return queued("kernel.waiters_addr") == 1; });
    CHECK_EQ(ks::signal_to_address(address(&word), 0, 0, 1), ks::kOk);
    job.finish(); CHECK_EQ(result, ks::kOk);
}
NXTEST(guest_workers_dispatch_lifted_instructions_with_their_own_registers) {
    Fixture fixture;
    REQUIRE(mem::reserve_fixed(mem::kMainBase, 0x10000, "kernel fixture"));
    REQUIRE(mem::commit(mem::kMainBase, 0x10000));
    for (int i = 0; i < nxmod_subpage_fixture.nfns; ++i)
        nxrt::fn_register(nxmod_subpage_fixture.fns[i].addr, nxmod_subpage_fixture.fns[i].fn);
    std::array<std::unique_ptr<Job>, 4> jobs;
    for (unsigned i = 0; i < jobs.size(); ++i) {
        jobs[i] = std::make_unique<Job>([i](::Ctx* c) {
            c->x[1] = mem::kMainBase + 0x8000 + 8 * i;
            c->x[0] = 0x12340000 + i;
            nxrt::dispatch_call(c, mem::kMainBase + 0x40); // Original ARM fixture's STLR.
            nxrt::dispatch_call(c, mem::kMainBase);        // LDAR.
        });
        jobs[i]->start();
    }
    for (unsigned i = 0; i < jobs.size(); ++i) {
        jobs[i]->finish();
        CHECK_EQ(*reinterpret_cast<uint64_t*>(mem::kMainBase + 0x8000 + 8 * i), 0x12340000u + i);
        CHECK_EQ(ks::handle_as<ks::KThread>(jobs[i]->handle)->ctx.x[0], 0x12340000u + i);
    }
}
