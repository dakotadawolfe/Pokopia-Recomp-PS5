// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/services.h"
#include "nxrt/svc.h"
#include "nxrecomp/ctx.h"
#include <array>
#include <atomic>
#include <thread>

NXTEST_GROUP(svc_info_ps5)
namespace nxrt { void svc_memory_reset_for_tests(); void svc_info_reset_for_tests(); }
namespace {
namespace k = nxrt::kernel;
namespace mem = nxrt::mem;
constexpr uint64_t process = k::kPseudoCurrentProcess, mib = 0x100000;
struct Result { uint64_t rc, value; };
Result call(unsigned svc, uint64_t a, uint64_t b = 0, uint64_t d = 0) {
    ::Ctx c{}; c.x[0] = UINT64_MAX; c.x[1] = a; c.x[2] = b; c.x[3] = d;
    nxrt::svc_dispatch(&c, svc); return {c.x[0], c.x[1]};
}
Result info(unsigned type, uint64_t handle = process, uint64_t sub = 0) { return call(0x29, type, handle, sub); }
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    Fixture() {
        k::kernel_reset(); nxrt::modules_unload_all(); nxrt::fn_clear();
        nxrt::svc_memory_reset_for_tests(); nxrt::svc_info_reset_for_tests(); mem::shutdown(); nxrt::config_clear();
        game.title_defaults = +[](nxrt::services::TitleProfile& p) { p.program_id = 0x0100000000001234; p.system_resource_size = 0x200000; };
        nxrt::game::set_current(&game); nxrt::svc_init();
        if (!mem::init({.heap_reserve = 64 * mib})) std::abort();
    }
    ~Fixture() {
        k::main_thread_release(); k::kernel_reset(); nxrt::modules_unload_all(); nxrt::fn_clear();
        nxrt::svc_memory_reset_for_tests(); nxrt::svc_info_reset_for_tests(); mem::shutdown(); nxrt::config_clear();
        nxrt::game::set_current(previous);
    }
};
}
NXTEST(info_svc_reports_the_guest_layout_and_platform) {
    Fixture f;
    for (const auto& [type, expected] : std::array<std::pair<unsigned, uint64_t>, 16>{{
        {0, 0x3f}, {1, 0x0ffffffff0000000}, {2, mem::kAliasBase}, {3, mem::kAliasSize},
        {4, mem::kHeapBase}, {5, mem::kHeapMax}, {12, 0x8000000}, {13, 0x7ff8000000},
        {14, mem::kStackBase}, {15, mem::kStackSize}, {28, 0}, {29, mem::kRemoteBase},
        {30, mem::kRemoteSize}, {33, 0x1000}, {36, 1ull << 39}, {23, 1}}}) {
        const auto r = info(type); CHECK_EQ(r.rc, k::kOk); CHECK_EQ(r.value, expected);
    }
    f.game.platform = nxrt::game::Platform::Switch1; CHECK_EQ(info(0).value, 7u);
}
NXTEST(info_svc_validates_process_handles_subtypes_and_unknown_types) {
    Fixture f;
    for (unsigned type : {0, 4, 6, 7, 12, 16, 18, 20, 23, 28, 29, 33, 36}) {
        const auto bad = info(type, 0); CHECK_EQ(bad.rc, k::kInvalidHandle); CHECK_EQ(bad.value, 0u);
        CHECK_EQ(info(type, process, 1).rc, k::kInvalidCombination);
    }
    CHECK_EQ(info(8, process).rc, k::kInvalidHandle);
    CHECK_EQ(info(8, 0).rc, k::kOk); CHECK_EQ(info(8, 0).value, 0u);
    CHECK_EQ(info(8, 0, 1).rc, k::kInvalidCombination);
    CHECK_EQ(info(0x1234).rc, k::kInvalidEnum); CHECK_EQ(info(0x1234).value, 0u);
}
NXTEST(info_svc_uses_one_budget_and_tracks_guest_heap_usage) {
    Fixture f; CHECK_EQ(info(6).value, 0x2a0000000ull); CHECK_EQ(info(21).value, info(6).value);
    CHECK_EQ(info(7).value, 64 * mib);
    REQUIRE(call(1, 4 * mib).rc == k::kOk);
    CHECK_EQ(info(7).value, 68 * mib); CHECK_EQ(info(22).value, 68 * mib);
    CHECK_EQ(call(0x6f, 0, 0, 0).value, info(6).value);
    CHECK_EQ(call(0x6f, 1, 0, 0).value, info(7).value);
    nxrt::config_set("memory", "app_memory_mb", "128"); CHECK_EQ(info(6).value, 128 * mib);
    CHECK_EQ(call(0x6f, 0, 0, 0).value, 128 * mib);
    REQUIRE(call(1, 0).rc == k::kOk); CHECK_EQ(info(7).value, 64 * mib);
}
NXTEST(info_svc_title_identity_respects_default_title_and_process_precedence) {
    Fixture f; CHECK_EQ(info(18).value, 0x0100000000001234ull); CHECK_EQ(info(16).value, 2 * mib);
    nxrt::config_set("title", "program_id", "0x2222"); nxrt::config_set("title", "system_resource_size", "4194304");
    CHECK_EQ(info(18).value, 0x2222u); CHECK_EQ(info(16).value, 4 * mib);
    nxrt::config_set("process", "program_id", "0x3333"); nxrt::config_set("process", "system_resource_size", "6291456");
    CHECK_EQ(info(18).value, 0x3333u); CHECK_EQ(info(16).value, 6 * mib);
    nxrt::config_set("process", "program_id", "0"); CHECK_EQ(info(18).value, 0u);
    CHECK_EQ(nxrt::services::title_defaults().supported_languages, 2u);
}
NXTEST(info_svc_exception_context_is_shared_zeroed_and_thread_safe) {
    Fixture f; std::array<uint64_t, 8> addresses{}; std::array<std::thread, 8> workers;
    for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
        auto r = info(20); addresses[i] = r.rc == k::kOk ? r.value : 0;
    });
    for (auto& worker : workers) worker.join(); REQUIRE(addresses[0]);
    for (auto a : addresses) CHECK_EQ(a, addresses[0]);
    CHECK(mem::is_mapped(addresses[0], mem::kPage)); CHECK_EQ(rd64(addresses[0]), 0u);
    wr64(addresses[0], 0x9876); CHECK_EQ(rd64(info(20).value), 0x9876u);
}
NXTEST(info_svc_exception_context_reset_does_not_reuse_a_thread_tls_page) {
    Fixture f; const auto old = info(20).value; REQUIRE(old);
    nxrt::svc_info_reset_for_tests(); mem::shutdown(); REQUIRE(mem::init({.heap_reserve = 64 * mib}));
    const auto occupied = mem::alloc_kernel(mem::kPage); REQUIRE(occupied); wr64(occupied, 0x1234);
    const auto fresh = info(20).value; REQUIRE(fresh); CHECK(fresh != occupied);
    CHECK_EQ(rd64(fresh), 0u); CHECK_EQ(rd64(occupied), 0x1234u);
}
NXTEST(info_svc_entropy_is_stable_and_checks_handle_and_platform_subtype) {
    Fixture f; std::array<uint64_t, 8> values{};
    for (unsigned i = 0; i < values.size(); ++i) { auto r = info(11, 0, i); CHECK_EQ(r.rc, k::kOk); values[i] = r.value; }
    for (unsigned i = 0; i < values.size(); ++i) {
        CHECK_EQ(info(11, 0, i).value, values[i]);
        for (unsigned j = i + 1; j < values.size(); ++j) CHECK(values[i] != values[j]);
    }
    CHECK_EQ(info(11, process).rc, k::kInvalidHandle); CHECK_EQ(info(11, 0, 8).rc, k::kInvalidCombination);
    f.game.platform = nxrt::game::Platform::Switch1; CHECK_EQ(info(11, 0, 4).rc, k::kInvalidCombination);
}
NXTEST(info_svc_process_identity_and_system_range_agree) {
    Fixture f; nxrt::config_set("process", "pid", "91");
    CHECK_EQ(call(0x24, process).value, 91u); CHECK_EQ(call(0x24, 0).rc, k::kInvalidHandle);
    CHECK_EQ(call(0x6f, 2, 0, 0).value, 91u); CHECK_EQ(call(0x6f, 2, 0, 1).value, 91u);
    CHECK_EQ(call(0x6f, 2, 0, 2).rc, k::kInvalidCombination);
    CHECK_EQ(call(0x6f, 0, process).rc, k::kInvalidHandle);
    CHECK_EQ(call(0x7c, process, 0).value, 2u); CHECK_EQ(call(0x7c, 0, 0).rc, k::kInvalidHandle);
    CHECK_EQ(call(0x7c, process, 1).rc, k::kInvalidCombination);
}
NXTEST(info_svc_thread_identity_tracks_the_adopted_guest_and_closed_handle) {
    Fixture f; CHECK_EQ(call(0x25, k::kPseudoCurrentThread).rc, k::kInvalidHandle);
    const auto handle = k::main_thread_adopt(0x10000); REQUIRE(handle);
    const auto id = k::current_thread()->id;
    CHECK_EQ(call(0x25, handle).value, id); CHECK_EQ(call(0x25, k::kPseudoCurrentThread).value, id);
    k::handle_close(handle); CHECK_EQ(call(0x25, handle).rc, k::kInvalidHandle);
    CHECK_EQ(call(0x25, k::kPseudoCurrentThread).value, id);
}
NXTEST(info_svc_process_copies_keep_identity_until_kernel_reset) {
    Fixture f; nxrt::config_set("process", "pid", "97");
    auto process_object = k::current_process();
    const auto copy = k::handle_add(process_object); REQUIRE(copy);
    CHECK_EQ(call(0x24, copy).value, 97u); CHECK_EQ(call(0x7c, copy).value, 2u);
    CHECK_EQ(info(18, copy).value, 0x0100000000001234ull);
    nxrt::config_set("process", "pid", "99");
    CHECK_EQ(call(0x24, process).value, 97u); CHECK_EQ(call(0x6f, 2, 0, 0).value, 97u);
    k::handle_close(copy); CHECK_EQ(call(0x24, copy).rc, k::kInvalidHandle);
    k::kernel_reset(); CHECK(k::current_process() != process_object); CHECK_EQ(k::current_process()->id, 99u);
    const auto stale = k::handle_add(process_object); CHECK_EQ(call(0x24, stale).rc, k::kInvalidHandle);
}
NXTEST(info_svc_reports_missing_accounting_as_unimplemented) {
    Fixture f; const auto handle = k::main_thread_adopt(0x10000); REQUIRE(handle);
    for (unsigned type : {9, 10}) CHECK_EQ(info(type, 0).rc, k::kNotImplemented);
    CHECK_EQ(info(17).rc, k::kNotImplemented);
    CHECK_EQ(info(25, handle, UINT64_MAX).rc, k::kNotImplemented);
    CHECK_EQ(info(25, handle, 9).rc, k::kInvalidCombination);
    CHECK_EQ(info(25, 0, UINT64_MAX).rc, k::kInvalidHandle);
    for (unsigned pool = 1; pool < 4; ++pool) CHECK_EQ(call(0x6f, 0, 0, pool).rc, k::kNotImplemented);
    for (unsigned svc : {0x30, 0x31, 0x37}) CHECK(!nxrt::svc_handler(svc));
}
