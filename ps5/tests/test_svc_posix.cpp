// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"
#include "nxrecomp/ctx.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

NXTEST_GROUP(svc_posix)
namespace ks = nxrt::kernel;
namespace mem = nxrt::mem;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t entry = 0x7100010000;
constexpr uint64_t fixture_base = 0x7300000000;
constexpr int64_t ms = 1000000;
extern "C" const NxModule nxmod_svc_fixture;

[[noreturn]] void watchdog(const char* message) {
    std::fprintf(stderr, "SVC TEST WATCHDOG: %s\n", message);
    std::fflush(stderr); std::abort();
}
template<class F> void until(F predicate) {
    const auto end = Clock::now() + 5s;
    while (!predicate()) {
        if (Clock::now() >= end) watchdog("wait condition did not become ready");
        std::this_thread::sleep_for(1ms);
    }
}
uint64_t addr(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint32_t call(::Ctx& c, uint32_t number) { nxrt::svc_dispatch(&c, number); return static_cast<uint32_t>(c.x[0]); }
uint32_t word(uint64_t p) { return __atomic_load_n(reinterpret_cast<uint32_t*>(p), __ATOMIC_SEQ_CST); }
int64_t queued(const char* name) {
    for (const auto& [key, value] : nxrt::gauge_read_all()) if (key == name) return value;
    return -2;
}
void join(uint32_t handle) { if (!ks::thread_join(handle, 5000)) watchdog("thread did not terminate"); }
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    const std::string old_stack = nxrt::config_get_str("threads", "host_stack_mb", "");
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    Fixture() {
        ks::kernel_reset(); nxrt::modules_unload_all(); nxrt::fn_clear(); mem::shutdown();
        nxrt::game::set_current(&game);
        nxrt::config_set("threads", "host_stack_mb", "2");
        if (!mem::init({.heap_reserve = 64ull << 20})) watchdog("memory initialization failed");
        nxrt::svc_init(); nxrt::svc_reset_counters();
        nxrt::fn_register(entry, +[](::Ctx* c) { (*reinterpret_cast<std::function<void(::Ctx*)>*>(c->x[0]))(c); });
        for (int i = 0; i < nxmod_svc_fixture.nfns; ++i)
            nxrt::fn_register(nxmod_svc_fixture.fns[i].addr, nxmod_svc_fixture.fns[i].fn);
    }
    ~Fixture() {
        ks::main_thread_release(); ks::kernel_reset();
        nxrt::modules_unload_all(); nxrt::fn_clear(); mem::shutdown();
        nxrt::svc_set_busy_hook(nullptr); nxrt::svc_register(0xf0, nullptr);
        nxrt::config_set("threads", "host_stack_mb", old_stack.c_str());
        nxrt::game::set_current(previous);
    }
};
struct Job {
    std::function<void(::Ctx*)> function;
    uint32_t handle;
    Job(std::function<void(::Ctx*)> f) : function(std::move(f)) {
        ks::ThreadParams p;
        p.entry = entry; p.arg = addr(&function); p.host_stack_bytes = 2 << 20;
        handle = ks::thread_create(p);
        if (!handle) watchdog("create failed");
    }
    void start() { if (ks::thread_start(handle)) watchdog("start failed"); }
    void finish() { join(handle); }
};
struct Event {
    uint32_t write, read;
    Event() {
        ::Ctx c{};
        if (call(c, 0x45)) watchdog("event creation failed");
        write = c.x[1]; read = c.x[2];
    }
    uint32_t signal() { ::Ctx c{}; c.x[0] = write; return call(c, 0x11); }
};
uint32_t wait_handle(uint32_t handle, int64_t timeout = 0) {
    const uint64_t buffer = mem::alloc_kernel(0x1000);
    if (!buffer) watchdog("handle buffer allocation failed");
    *reinterpret_cast<uint32_t*>(buffer) = handle;
    ::Ctx c{}; c.x[1] = buffer; c.x[2] = 1; c.x[3] = timeout;
    return call(c, 0x18);
}
}

NXTEST(svc_registers_only_the_ported_services) {
    Fixture fixture;
    unsigned count = 0;
    for (unsigned i = 0; i < nxrt::kSvcCount; ++i) count += nxrt::svc_handler(i) != nullptr;
    CHECK_EQ(count, 23u);
    CHECK_EQ(std::string(nxrt::svc_name(0x18)), "WaitSynchronization");
    CHECK_EQ(std::string(nxrt::svc_name(0x45)), "CreateEvent");
    for (unsigned missing : {0x01, 0x07, 0x10, 0x1f, 0x21, 0x26, 0x28, 0x29, 0x32, 0x36}) {
        CHECK(!nxrt::svc_handler(missing));
        ::Ctx c{}; c.x[0] = UINT64_MAX;
        CHECK_EQ(call(c, missing), ks::kNotImplemented);
        CHECK_EQ(c.x[0], uint64_t(ks::kNotImplemented));
    }
}
NXTEST(unknown_svc_keeps_arguments_and_records_the_original_call_site) {
    Fixture fixture;
    ::Ctx c{};
    for (unsigned i = 0; i < 31; ++i) c.x[i] = 0x12340000 + i;
    c.fn = mem::kMainBase + 0x400;
    nxrt::svc_dispatch(&c, 0x1234, mem::kMainBase + 0x408);
    CHECK_EQ(c.x[0], uint64_t(ks::kNotImplemented));
    for (unsigned i = 1; i < 31; ++i) CHECK_EQ(c.x[i], 0x12340000u + i);
    const auto records = nxrt::svc_unknown_records();
    REQUIRE(records.size() == 1);
    CHECK_EQ(records[0].number, 0x1234u);
    CHECK_EQ(records[0].first_args[0], 0x12340000u);
    CHECK_EQ(records[0].first_fn, mem::kMainBase + 0x400);
    CHECK_EQ(records[0].first_pc, mem::kMainBase + 0x408);
    CHECK_EQ(nxrt::svc_unknown_total(), 1u);
    CHECK(nxrt::svc_summary().find("1 unknown") != std::string::npos);
}
NXTEST(svc_handler_publication_is_safe_during_concurrent_dispatch) {
    Fixture fixture;
    constexpr auto first = +[](::Ctx* c) { c->x[0] = 0x1111; };
    constexpr auto second = +[](::Ctx* c) { c->x[0] = 0x2222; };
    nxrt::svc_register(0xf0, first);
    std::atomic<unsigned> invalid{0};
    std::thread reader([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            ::Ctx c{}; const auto rc = call(c, 0xf0);
            if (rc != 0x1111 && rc != 0x2222) ++invalid;
        }
    });
    for (unsigned i = 0; i < 10000; ++i) nxrt::svc_register(0xf0, i & 1 ? first : second);
    reader.join();
    CHECK_EQ(invalid.load(), 0u);
    CHECK_EQ(nxrt::svc_call_count(0xf0), 10000u);
}
NXTEST(lifted_svc_reads_the_tick_and_platform_frequency_through_the_real_abi) {
    Fixture fixture;
    ::Ctx c{}; c.x[2] = 0xdeadbeef;
    const auto begin = ks::system_tick();
    nxrt::dispatch_call(&c, fixture_base);
    CHECK(c.x[0] >= begin && c.x[0] <= ks::system_tick());
    CHECK_EQ(c.x[1], 31250000u);
    CHECK_EQ(c.x[2], 0xdeadbeefu);
    CHECK_EQ(nxrt::svc_call_count(0x1e), 1u);
    CHECK_EQ(nxrt::svc_unknown_total(), 0u);
}
NXTEST(event_handles_enforce_read_write_roles_and_manual_reset) {
    Fixture fixture;
    Event event;
    ::Ctx c{}; c.x[0] = event.read;
    CHECK_EQ(call(c, 0x11), ks::kInvalidHandle);
    c.x[0] = event.write;
    CHECK_EQ(call(c, 0x17), ks::kInvalidHandle);
    CHECK_EQ(event.signal(), ks::kOk);
    CHECK_EQ(wait_handle(event.read), ks::kOk);
    CHECK_EQ(wait_handle(event.read), ks::kOk);
    CHECK_EQ(wait_handle(event.write), ks::kInvalidHandle);
    c.x[0] = event.read;
    CHECK_EQ(call(c, 0x17), ks::kOk);
    c.x[0] = event.read;
    CHECK_EQ(call(c, 0x17), ks::kInvalidState);
    CHECK_EQ(wait_handle(event.read), ks::kTimedOut);
    CHECK_EQ(event.signal(), ks::kOk);
    c.x[0] = event.write; CHECK_EQ(call(c, 0x12), ks::kOk);
    CHECK_EQ(wait_handle(event.read), ks::kTimedOut);
}
NXTEST(concurrent_event_resets_consume_one_signal_once) {
    Fixture fixture;
    Event event; CHECK_EQ(event.signal(), ks::kOk);
    std::atomic<unsigned> ready{0}, success{0}, invalid{0};
    std::atomic<bool> release{false};
    std::array<std::thread, 4> workers;
    for (auto& thread : workers) thread = std::thread([&] {
        ++ready; while (!release) std::this_thread::yield();
        ::Ctx c{}; c.x[0] = event.read;
        const auto rc = call(c, 0x17);
        if (rc == ks::kOk) ++success;
        else if (rc != ks::kInvalidState) ++invalid;
    });
    until([&] { return ready.load() == 4; }); release = true;
    for (auto& thread : workers) thread.join();
    CHECK_EQ(success.load(), 1u); CHECK_EQ(invalid.load(), 0u);
}
NXTEST(event_creation_rolls_back_the_first_handle_if_the_second_cannot_fit) {
    Fixture fixture;
    auto object = std::make_shared<ks::KObject>();
    for (unsigned i = 0; i < 0x2fff; ++i) REQUIRE(ks::handle_add(object));
    ::Ctx c{}; c.x[1] = 0x1234; c.x[2] = 0x5678;
    CHECK_EQ(call(c, 0x45), ks::kOutOfHandles);
    CHECK_EQ(ks::handle_count(), 0x2fffu);
    CHECK_EQ(c.x[1], 0x1234u); CHECK_EQ(c.x[2], 0x5678u);
}
NXTEST(wait_svc_validates_guest_buffers_ranges_and_object_types) {
    Fixture fixture;
    ::Ctx c{}; c.x[1] = 0; c.x[2] = 0; c.x[3] = 0;
    CHECK_EQ(call(c, 0x18), ks::kTimedOut);
    c.x[1] = UINT64_MAX - 1; c.x[2] = 2;
    CHECK_EQ(call(c, 0x18), ks::kInvalidPointer);
    c.x[2] = 65; CHECK_EQ(call(c, 0x18), ks::kOutOfRange);
    c.x[2] = UINT32_MAX; CHECK_EQ(call(c, 0x18), ks::kOutOfRange);
    const auto data = mem::alloc_kernel(0x2000, 0x4000);
    REQUIRE(data);
    REQUIRE(mem::protect(data + 0x1000, 0x1000, mem::Prot::None));
    c.x[1] = data + 0xffe; c.x[2] = 1;
    CHECK_EQ(call(c, 0x18), ks::kInvalidPointer);
    const auto h = ks::handle_add(std::make_shared<ks::KObject>());
    CHECK_EQ(wait_handle(h), ks::kInvalidHandle);
}
NXTEST(event_wait_survives_read_handle_close_after_the_wait_has_started) {
    Fixture fixture;
    Event event;
    const auto data = mem::alloc_kernel(0x1000);
    REQUIRE(data);
    *reinterpret_cast<uint32_t*>(data) = event.read;
    uint32_t result = 0;
    Job job([&](::Ctx* c) { c->x[1] = data; c->x[2] = 1; c->x[3] = 5000 * ms; result = call(*c, 0x18); });
    job.start(); until([&] { return queued("kernel.waiters_sync") == 1; });
    ::Ctx c{}; c.x[0] = event.read; CHECK_EQ(call(c, 0x16), ks::kOk);
    CHECK_EQ(event.signal(), ks::kOk); job.finish();
    CHECK_EQ(result, ks::kOk);
    c.x[0] = event.read; CHECK_EQ(call(c, 0x16), ks::kInvalidHandle);
    c.x[0] = ks::kPseudoCurrentThread; CHECK_EQ(call(c, 0x16), ks::kInvalidHandle);
}
NXTEST(create_thread_svc_rejects_bad_priority_and_core_without_leaking_handles) {
    Fixture fixture;
    ::Ctx c{}; c.x[4] = 64; c.x[5] = 0;
    CHECK_EQ(call(c, 0x08), ks::kInvalidPriority);
    c.x[4] = 44; c.x[5] = uint32_t(-1);
    CHECK_EQ(call(c, 0x08), ks::kInvalidCoreId);
    c.x[5] = 8; CHECK_EQ(call(c, 0x08), ks::kInvalidCoreId);
    CHECK_EQ(ks::handle_count(), 0u);
}
NXTEST(thread_svc_preserves_arguments_and_dispatches_the_created_context) {
    Fixture fixture;
    uint64_t actual_arg = 0, actual_stack = 0;
    nxrt::fn_register(entry + 0x40, +[](::Ctx* c) {
        auto* output = reinterpret_cast<uint64_t*>(c->x[0]);
        output[0] = c->x[0]; output[1] = c->x[31];
    });
    std::array<uint64_t, 2> output{};
    const auto stack = mem::alloc_kernel(0x10000);
    ::Ctx c{}; c.x[1] = entry + 0x40; c.x[2] = addr(output.data()); c.x[3] = stack + 0x10000;
    c.x[4] = 44; c.x[5] = uint32_t(-2);
    CHECK_EQ(call(c, 0x08), ks::kOk);
    const auto handle = static_cast<uint32_t>(c.x[1]); REQUIRE(handle);
    c.x[0] = handle; CHECK_EQ(call(c, 0x09), ks::kOk); join(handle);
    actual_arg = output[0]; actual_stack = output[1];
    CHECK_EQ(actual_arg, addr(output.data())); CHECK_EQ(actual_stack, stack + 0x10000);
    c.x[0] = handle; CHECK_EQ(call(c, 0x09), ks::kInvalidState);
    c.x[1] = handle; CHECK_EQ(call(c, 0x0c), ks::kOk); CHECK_EQ(c.x[1], 44u);
    c.x[0] = handle; c.x[1] = 28; CHECK_EQ(call(c, 0x0d), ks::kOk);
    c.x[1] = handle; CHECK_EQ(call(c, 0x0c), ks::kOk); CHECK_EQ(c.x[1], 28u);
    c.x[0] = handle; c.x[1] = 5; c.x[2] = 0x30; CHECK_EQ(call(c, 0x0f), ks::kOk);
    c.x[2] = handle; CHECK_EQ(call(c, 0x0e), ks::kOk); CHECK_EQ(c.x[1], 5u); CHECK_EQ(c.x[2], 0x30u);
}
NXTEST(cancel_svc_wakes_a_guest_blocked_in_wait_synchronization) {
    Fixture fixture;
    uint32_t result = 0;
    Job job([&](::Ctx* c) { c->x[1] = 0; c->x[2] = 0; c->x[3] = UINT64_MAX; result = call(*c, 0x18); });
    job.start(); until([&] { return queued("kernel.waiters_sync") == 1; });
    ::Ctx c{}; c.x[0] = job.handle; CHECK_EQ(call(c, 0x19), ks::kOk);
    job.finish(); CHECK_EQ(result, ks::kCancelled);
}
NXTEST(word_svcs_reject_unmapped_and_read_only_guest_pages_before_writing) {
    Fixture fixture;
    const auto data = mem::alloc_kernel(0x4000, 0x4000);
    REQUIRE(data);
    *reinterpret_cast<uint32_t*>(data) = 7;
    *reinterpret_cast<uint32_t*>(data + 0x1000) = 9;
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::R));
    ::Ctx c{}; c.x[0] = data; c.x[1] = 1; c.x[2] = 8; c.x[3] = 0;
    CHECK_EQ(call(c, 0x34), ks::kInvalidMemoryState);
    c.x[0] = data; c.x[1] = 2; c.x[2] = 7;
    CHECK_EQ(call(c, 0x34), ks::kTimedOut);
    c.x[0] = data; c.x[1] = 1; c.x[2] = 7; c.x[3] = 1;
    CHECK_EQ(call(c, 0x35), ks::kInvalidMemoryState);
    c.x[0] = data; CHECK_EQ(call(c, 0x1b), ks::kInvalidMemoryState);
    c.x[0] = 1; c.x[1] = data; c.x[2] = 2;
    CHECK_EQ(call(c, 0x1a), ks::kInvalidMemoryState);
    c.x[0] = mem::kMainBase; CHECK_EQ(call(c, 0x1b), ks::kInvalidMemoryState);
    c.x[0] = data + 1; CHECK_EQ(call(c, 0x1b), ks::kInvalidAddress);
    CHECK_EQ(word(data), 7u); CHECK_EQ(word(data + 0x1000), 9u);
}
NXTEST(process_wide_key_svc_hands_back_the_mutex_and_preserves_void_registers) {
    Fixture fixture;
    const auto data = mem::alloc_kernel(0x1000);
    REQUIRE(data);
    uint32_t result = 0;
    Job job([&](::Ctx* c) {
        c->x[0] = data; c->x[1] = data + 4; c->x[2] = ks::current_thread()->handle; c->x[3] = 5000 * ms;
        result = call(*c, 0x1c);
    });
    *reinterpret_cast<uint32_t*>(data) = job.handle;
    job.start(); until([&] { return queued("kernel.waiters_cv") == 1; });
    CHECK_EQ(word(data), 0u); CHECK_EQ(word(data + 4), 1u);
    ::Ctx c{}; c.x[0] = data + 4; c.x[1] = 1; c.x[2] = 0xfeed;
    nxrt::svc_dispatch(&c, 0x1d);
    CHECK_EQ(c.x[0], data + 4); CHECK_EQ(c.x[1], 1u); CHECK_EQ(c.x[2], 0xfeedu);
    job.finish(); CHECK_EQ(result, ks::kOk); CHECK_EQ(word(data), job.handle);
    c.x[0] = data; CHECK_EQ(call(c, 0x1b), ks::kOk); CHECK_EQ(word(data), 0u);
}
NXTEST(address_svcs_wait_decrement_and_signal_through_the_dispatcher) {
    Fixture fixture;
    const auto data = mem::alloc_kernel(0x1000);
    REQUIRE(data);
    uint32_t result = 0;
    Job job([&](::Ctx* c) { c->x[0] = data; c->x[1] = 1; c->x[2] = 1; c->x[3] = 5000 * ms; result = call(*c, 0x34); });
    job.start(); until([&] { return queued("kernel.waiters_addr") == 1; });
    CHECK_EQ(word(data), UINT32_MAX);
    ::Ctx c{}; c.x[0] = data; c.x[1] = 1; c.x[2] = UINT32_MAX; c.x[3] = 1;
    CHECK_EQ(call(c, 0x35), ks::kOk); job.finish();
    CHECK_EQ(result, ks::kOk); CHECK_EQ(word(data), 0u);
}
NXTEST(debug_string_svc_checks_guest_permissions_and_accepts_empty_text) {
    Fixture fixture;
    ::Ctx c{}; c.x[0] = 0; c.x[1] = 0;
    CHECK_EQ(call(c, 0x27), ks::kOk);
    c.x[0] = UINT64_MAX; c.x[1] = 12;
    CHECK_EQ(call(c, 0x27), ks::kInvalidPointer);
    const auto data = mem::alloc_kernel(0x2000, 0x4000);
    REQUIRE(data);
    std::memcpy(reinterpret_cast<void*>(data), "runtime message\n", 16);
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::R));
    c.x[0] = data; c.x[1] = 16; CHECK_EQ(call(c, 0x27), ks::kOk);
    REQUIRE(mem::protect(data + 0x1000, 0x1000, mem::Prot::None));
    c.x[0] = data + 0xffc; c.x[1] = 16; CHECK_EQ(call(c, 0x27), ks::kInvalidPointer);
}
NXTEST(sleep_svc_waits_without_overwriting_the_argument_register) {
    Fixture fixture;
    ::Ctx c{}; c.x[0] = 20 * ms; c.x[1] = 0x1234;
    const auto begin = Clock::now(); nxrt::svc_dispatch(&c, 0x0b);
    CHECK(Clock::now() - begin >= 19ms);
    CHECK_EQ(c.x[0], uint64_t(20 * ms)); CHECK_EQ(c.x[1], 0x1234u);
    for (int64_t yield : {0, -1, -2}) {
        c.x[0] = static_cast<uint64_t>(yield); nxrt::svc_dispatch(&c, 0x0b);
        CHECK_EQ(c.x[0], static_cast<uint64_t>(yield));
    }
}
NXTEST(lifted_arm_parent_creates_waits_and_closes_its_child_thread) {
    Fixture fixture;
    const auto data = mem::alloc_kernel(0x1000);
    const auto stack = mem::alloc_kernel(0x10000);
    REQUIRE(data && stack);
    std::memset(reinterpret_cast<void*>(data), 0xcd, 64);
    *reinterpret_cast<uint64_t*>(data) = stack + 0x10000;
    ks::ThreadParams p;
    p.entry = fixture_base + 0x40; p.arg = data; p.host_stack_bytes = 2 << 20;
    const auto parent = ks::thread_create(p); REQUIRE(parent);
    CHECK_EQ(ks::thread_start(parent), ks::kOk); join(parent);
    for (unsigned offset : {8, 16, 20, 24, 28}) CHECK_EQ(word(data + offset), 0u);
    CHECK(!ks::handle_get(word(data + 12)));
    CHECK_EQ(word(data + 48), 0x55aau); // The instruction after ExitThread never ran.
    const auto child_tls = *reinterpret_cast<uint64_t*>(data + 32);
    const auto child_tp = *reinterpret_cast<uint64_t*>(data + 40);
    CHECK(child_tls && child_tp);
    CHECK_EQ(*reinterpret_cast<uint64_t*>(child_tls + 0x1f0), child_tp);
    CHECK(child_tls != ks::handle_as<ks::KThread>(parent)->tls);
    CHECK_EQ(nxrt::svc_call_count(0x08), 1u); CHECK_EQ(nxrt::svc_call_count(0x0a), 1u);
    CHECK_EQ(nxrt::svc_unknown_total(), 0u);
}
NXTEST(lifted_arm_event_program_completes_a_full_signal_wait_reset_cycle) {
    Fixture fixture;
    const auto data = mem::alloc_kernel(0x1000); REQUIRE(data);
    std::memset(reinterpret_cast<void*>(data), 0xcd, 32);
    ::Ctx c{}; c.x[0] = data;
    nxrt::dispatch_call(&c, fixture_base + 0x300);
    for (unsigned offset : {0, 12, 16, 20, 24}) CHECK_EQ(word(data + offset), 0u);
    CHECK_EQ(word(data + 28), ks::kTimedOut);
    CHECK_EQ(ks::handle_count(), 0u);
    CHECK_EQ(nxrt::svc_unknown_total(), 0u);
}
