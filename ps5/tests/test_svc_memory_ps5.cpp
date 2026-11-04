// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "protect_fault.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/subpage.h"
#include "nxrt/svc.h"
#include "nxrecomp/ctx.h"
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

NXTEST_GROUP(svc_memory_ps5)
namespace nxrt { void svc_memory_reset_for_tests(); }
namespace {
namespace mem = nxrt::mem;
namespace k = nxrt::kernel;
constexpr uint64_t page = 0x1000, mib = 0x100000, heap = mem::kHeapBase, image = mem::kMainBase;
extern "C" const NxModule nxmod_svc_fixture;
struct Result { uint64_t rc, x1; };
Result call(uint32_t n, uint64_t x0 = 0, uint64_t x1 = 0, uint64_t x2 = 0, uint64_t x3 = 0) {
    ::Ctx c{}; c.x[0] = x0; c.x[1] = x1; c.x[2] = x2; c.x[3] = x3;
    nxrt::svc_dispatch(&c, n); return {c.x[0], c.x[1]};
}
Result resize(uint64_t size) { return call(1, 0, size); }
Result permission(uint64_t at, uint64_t size, unsigned perm) { return call(2, at, size, perm); }
Result attribute(uint64_t at, uint64_t size, unsigned mask, unsigned value) { return call(3, at, size, mask, value); }
struct Info { uint64_t base, size; uint32_t state, attr, perm, ipc, device, padding; };
static_assert(sizeof(Info) == 40);
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    const std::string budget = nxrt::config_get_str("memory", "app_memory_mb", "");
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    uint64_t output = 0;
    Fixture() {
        k::kernel_reset(); nxrt::modules_unload_all(); nxrt::fn_clear();
        nxrt::svc_memory_reset_for_tests(); mem::shutdown();
        nxrt::game::set_current(&game); nxrt::config_set("memory", "app_memory_mb", "0");
        if (!mem::init({.heap_reserve = 64 * mib}) || !(output = mem::alloc_kernel(4 * page, 4 * page))) std::abort();
        nxrt::svc_init(); nxrt::svc_reset_counters();
    }
    ~Fixture() {
        test_fail_protect = 0; k::kernel_reset(); nxrt::modules_unload_all(); nxrt::fn_clear();
        nxrt::svc_memory_reset_for_tests(); mem::shutdown();
        nxrt::config_set("memory", "app_memory_mb", budget.c_str()); nxrt::game::set_current(previous);
    }
    Info query(uint64_t address) {
        const auto result = call(6, output, 0xdead, address);
        CHECK_EQ(result.rc, k::kOk); CHECK_EQ(result.x1, 0u);
        Info info{}; std::memcpy(&info, reinterpret_cast<void*>(output), sizeof info); return info;
    }
    void module() {
        if (!mem::reserve_fixed(image, 0x10000, "SVC module fixture") || !mem::commit(image, 0x10000)) std::abort();
        mem::vm_set(image, 0x10000, mem::kMemCodeData, 3);
    }
};
}

NXTEST(heap_svc_grows_preserves_prefix_and_zeroes_regrown_tail) {
    Fixture f;
    CHECK_EQ(nxrt::svc_current_heap_size(), 0u);
    REQUIRE(resize(2 * mib).rc == k::kOk); CHECK_EQ(resize(2 * mib).x1, heap);
    wr64(heap, 0x12345678); CHECK_EQ(rd64(heap + 2 * mib - 8), 0u);
    REQUIRE(resize(4 * mib).rc == k::kOk); wr64(heap + 3 * mib, 0xabcdef);
    CHECK_EQ(rd64(heap), 0x12345678u);
    REQUIRE(resize(2 * mib).rc == k::kOk);
    CHECK_EQ(f.query(heap + 3 * mib).state, mem::kMemFree);
    CHECK(!mem::SubpageMap::allows(heap + 3 * mib, 8, 1));
    REQUIRE(resize(4 * mib).rc == k::kOk);
    CHECK_EQ(rd64(heap + 3 * mib), 0u); CHECK_EQ(rd64(heap), 0x12345678u);
    REQUIRE(resize(0).rc == k::kOk); CHECK_EQ(nxrt::svc_current_heap_size(), 0u);
    CHECK_EQ(mem::heap_committed(), 0u);
}
NXTEST(heap_svc_rejects_sizes_without_changing_live_memory) {
    Fixture f; REQUIRE(resize(2 * mib).rc == k::kOk); wr64(heap, 17);
    CHECK_EQ(resize(2 * mib + page).rc, k::kInvalidSize);
    CHECK_EQ(resize(UINT64_MAX).rc, k::kInvalidSize);
    CHECK_EQ(resize(mem::kHeapMax + 2 * mib).rc, k::kOutOfMemory);
    CHECK_EQ(resize(66 * mib).rc, k::kOutOfMemory); // Larger than this fixture's reservation.
    CHECK_EQ(nxrt::svc_current_heap_size(), 2 * mib); CHECK_EQ(rd64(heap), 17u);
}
NXTEST(heap_svc_honors_guest_resource_budget_and_platform_limits) {
    Fixture f; CHECK_EQ(nxrt::svc_total_memory(), 0x2a0000000ull);
    f.game.platform = nxrt::game::Platform::Switch1;
    CHECK_EQ(nxrt::svc_total_memory(), 0xcd500000ull);
    CHECK_EQ(resize(0x100200000ull).rc, k::kOutOfMemory);
    f.game.platform = nxrt::game::Platform::Switch2;
    nxrt::config_set("memory", "app_memory_mb", "66");
    REQUIRE(resize(2 * mib).rc == k::kOk);
    CHECK_EQ(resize(4 * mib).rc, k::kOutOfMemory); CHECK_EQ(nxrt::svc_current_heap_size(), 2 * mib);
    nxrt::config_set("memory", "app_memory_mb", "9223372036854775807");
    CHECK_EQ(nxrt::svc_total_memory(), UINT64_MAX); // Conversion saturates instead of wrapping.
}
NXTEST(permission_svc_restores_no_access_and_preserves_adjacent_guest_pages) {
    Fixture f; REQUIRE(resize(2 * mib).rc == k::kOk); wr32(heap, 71); wr32(heap + page, 82);
    REQUIRE(permission(heap, page, 0).rc == k::kOk);
    CHECK(!mem::SubpageMap::allows(heap, 4, 1)); CHECK(mem::SubpageMap::allows(heap + page, 4, 3));
    CHECK_EQ(f.query(heap).perm, 0u); CHECK_EQ(f.query(heap + page).perm, 3u);
    REQUIRE(permission(heap, page, 1).rc == k::kOk);
    CHECK_EQ(rd32(heap), 71u); CHECK(!mem::SubpageMap::allows(heap, 4, 2));
    REQUIRE(permission(heap, page, 3).rc == k::kOk); wr32(heap, 93);
    CHECK_EQ(rd32(heap), 93u); CHECK_EQ(rd32(heap + page), 82u);
}
NXTEST(permission_svc_rejects_holes_overflow_and_invalid_permissions) {
    Fixture f; f.module();
    CHECK_EQ(permission(image + 1, page, 1).rc, k::kInvalidAddress);
    CHECK_EQ(permission(image, 0, 1).rc, k::kInvalidSize);
    CHECK_EQ(permission(image, page + 1, 1).rc, k::kInvalidSize);
    CHECK_EQ(permission(image, page, 7).rc, k::kInvalidPermission);
    CHECK_EQ(permission(UINT64_MAX - page + 1, 2 * page, 1).rc, k::kInvalidMemoryState);
    CHECK_EQ(permission(image, 0x11000, 1).rc, k::kInvalidMemoryState);
    CHECK_EQ(permission(mem::kStackBase, page, 1).rc, k::kInvalidMemoryState);
    CHECK(mem::SubpageMap::allows(image, 0x10000, 3)); CHECK_EQ(f.query(image).perm, 3u);
}
NXTEST(permission_svc_keeps_code_data_state_and_rejects_code_text) {
    Fixture f; f.module();
    REQUIRE(permission(image, 0x4000, 1).rc == k::kOk);
    CHECK_EQ(f.query(image).state, mem::kMemCodeData); CHECK_EQ(f.query(image).perm, 1u);
    mem::vm_set(image + 0x4000, 0x4000, mem::kMemCode, 5);
    CHECK_EQ(permission(image + 0x4000, page, 3).rc, k::kInvalidMemoryState);
    CHECK_EQ(permission(image, 0x8000, 3).rc, k::kInvalidMemoryState);
    CHECK_EQ(f.query(image).perm, 1u); CHECK_EQ(f.query(image + 0x4000).state, mem::kMemCode);
}
NXTEST(query_svc_checks_the_entire_output_without_partial_writes) {
    Fixture f; REQUIRE(resize(2 * mib).rc == k::kOk);
    std::memset(reinterpret_cast<void*>(f.output), 0x5a, 4 * page);
    REQUIRE(mem::protect(f.output + page, page, mem::Prot::R));
    CHECK_EQ(call(6, f.output + page - 16, 0, heap).rc, k::kInvalidPointer);
    for (unsigned i = 0; i < 16; ++i) CHECK_EQ(rd8(f.output + page - 16 + i), 0x5au);
    for (uint64_t address : {uint64_t{0}, f.output + page, UINT64_MAX - 20, mem::kStackBase})
        CHECK_EQ(call(6, address, 0, heap).rc, k::kInvalidPointer);
    Info native{}; CHECK_EQ(call(6, reinterpret_cast<uint64_t>(&native), 0, heap).rc, k::kInvalidPointer);
    CHECK_EQ(call(6, f.output + 1, 0, heap).rc, k::kOk); // Byte-copy output accepts unaligned buffers.
}
NXTEST(query_svc_reports_heap_free_gaps_and_outside_address_space) {
    Fixture f; REQUIRE(resize(2 * mib).rc == k::kOk);
    auto info = f.query(heap + page); CHECK_EQ(info.base, heap); CHECK_EQ(info.size, 2 * mib);
    CHECK_EQ(info.state, mem::kMemHeap); CHECK_EQ(info.perm, 3u);
    CHECK_EQ(info.attr | info.ipc | info.device | info.padding, 0u);
    info = f.query(heap + 2 * mib); CHECK_EQ(info.state, mem::kMemFree); CHECK_EQ(info.base, heap + 2 * mib);
    info = f.query(UINT64_MAX); CHECK_EQ(info.state, mem::kMemInaccessible);
    CHECK_EQ(info.base, 1ull << 39); CHECK_EQ(info.size, uint64_t{0} - (1ull << 39));
}
NXTEST(relro_permission_lock_splits_query_regions_and_prevents_reprotection) {
    Fixture f; f.module();
    REQUIRE(attribute(image + page, page, 16, 16).rc == k::kOk);
    auto before = f.query(image), locked = f.query(image + page), after = f.query(image + 2 * page);
    CHECK_EQ(before.base, image); CHECK_EQ(before.size, page); CHECK_EQ(before.attr, 0u);
    CHECK_EQ(locked.base, image + page); CHECK_EQ(locked.size, page); CHECK_EQ(locked.attr, 16u);
    CHECK_EQ(after.base, image + 2 * page); CHECK_EQ(after.size, 0xe000u); CHECK_EQ(after.attr, 0u);
    CHECK_EQ(permission(image + page, page, 1).rc, k::kInvalidMemoryState);
    CHECK_EQ(permission(image, 3 * page, 1).rc, k::kInvalidMemoryState);
    CHECK_EQ(f.query(image).perm, 3u);
    REQUIRE(permission(image, page, 1).rc == k::kOk); REQUIRE(permission(image + 2 * page, page, 1).rc == k::kOk);
}
NXTEST(attribute_svc_rejects_invalid_masks_ranges_and_unported_cache_changes) {
    Fixture f; f.module(); REQUIRE(resize(2 * mib).rc == k::kOk);
    CHECK_EQ(attribute(image, page, 16, 0).rc, k::kInvalidCombination);
    CHECK_EQ(attribute(image, page, 0, 16).rc, k::kInvalidCombination);
    CHECK_EQ(attribute(image, page, 32, 32).rc, k::kInvalidCombination);
    CHECK_EQ(attribute(image + 1, page, 16, 16).rc, k::kInvalidAddress);
    CHECK_EQ(attribute(image, 0, 16, 16).rc, k::kInvalidSize);
    CHECK_EQ(attribute(heap, page, 16, 16).rc, k::kInvalidMemoryState);
    CHECK_EQ(attribute(mem::kStackBase, page, 0, 0).rc, k::kInvalidMemoryState);
    CHECK_EQ(attribute(image, page, 8, 8).rc, k::kNotImplemented);
    CHECK_EQ(attribute(image, page, 24, 24).rc, k::kNotImplemented); CHECK_EQ(f.query(image).attr, 0u);
    CHECK_EQ(attribute(image, page, 0, 0).rc, k::kOk);
}
NXTEST(repeated_adjacent_and_overlapping_permission_locks_merge) {
    Fixture f; f.module();
    REQUIRE(attribute(image + page, page, 16, 16).rc == k::kOk);
    REQUIRE(attribute(image + page, page, 16, 16).rc == k::kOk);
    REQUIRE(attribute(image + 3 * page, page, 16, 16).rc == k::kOk);
    REQUIRE(attribute(image + 2 * page, page, 16, 16).rc == k::kOk);
    REQUIRE(attribute(image, 2 * page, 16, 16).rc == k::kOk);
    const auto merged = f.query(image + 2 * page);
    CHECK_EQ(merged.base, image); CHECK_EQ(merged.size, 4 * page); CHECK_EQ(merged.attr, 16u);
    CHECK_EQ(f.query(image + 4 * page).attr, 0u);
    CHECK_EQ(permission(image + 3 * page, page, 1).rc, k::kInvalidMemoryState);
}
NXTEST(permission_svc_rolls_back_a_native_failure_before_changing_metadata) {
    Fixture f; f.module(); wr64(image, 19);
    test_fail_protect = 1; CHECK_EQ(permission(image, 0x10000, 1).rc, k::kInvalidMemoryState);
    CHECK_EQ(test_fail_protect.load(), 0u); CHECK_EQ(f.query(image).perm, 3u);
    CHECK(mem::SubpageMap::allows(image, 0x10000, 3)); CHECK_EQ(rd64(image), 19u);
    CHECK_EQ(permission(image, 0x10000, 1).rc, k::kOk);
}
NXTEST(heap_svc_failed_growth_keeps_the_previous_size_and_contents) {
    Fixture f; REQUIRE(resize(2 * mib).rc == k::kOk); wr64(heap, 37);
    test_fail_protect = 1; CHECK_EQ(resize(4 * mib).rc, k::kOutOfMemory);
    CHECK_EQ(test_fail_protect.load(), 0u); CHECK_EQ(nxrt::svc_current_heap_size(), 2 * mib);
    CHECK_EQ(rd64(heap), 37u); CHECK_EQ(f.query(heap + 2 * mib).state, mem::kMemFree);
    CHECK_EQ(mem::heap_committed(), 2 * mib); CHECK_EQ(resize(4 * mib).rc, k::kOk);
}
NXTEST(heap_svc_failed_shrink_keeps_the_tail_accessible) {
    Fixture f; REQUIRE(resize(4 * mib).rc == k::kOk); wr64(heap + 3 * mib, 49);
    test_fail_protect = 1; CHECK_EQ(resize(2 * mib).rc, k::kInvalidMemoryState);
    CHECK_EQ(test_fail_protect.load(), 0u); CHECK_EQ(nxrt::svc_current_heap_size(), 4 * mib);
    CHECK_EQ(rd64(heap + 3 * mib), 49u); CHECK_EQ(f.query(heap + 3 * mib).state, mem::kMemHeap);
    CHECK_EQ(resize(2 * mib).rc, k::kOk);
}
NXTEST(heap_svc_refuses_to_remove_protected_or_aliased_tail_pages) {
    Fixture f; REQUIRE(resize(4 * mib).rc == k::kOk);
    REQUIRE(permission(heap + 2 * mib, 2 * mib, 1).rc == k::kOk);
    CHECK_EQ(resize(2 * mib).rc, k::kInvalidMemoryState);
    REQUIRE(permission(heap + 2 * mib, 2 * mib, 3).rc == k::kOk);
    const auto alias = mem::heap_alias(2 * mib, 0x10000); REQUIRE(alias);
    CHECK_EQ(resize(2 * mib).rc, k::kInvalidMemoryState);
    REQUIRE(mem::heap_alias_unmap(alias)); CHECK_EQ(resize(2 * mib).rc, k::kOk);
}
NXTEST(memory_svcs_serialize_concurrent_heap_changes_and_queries) {
    Fixture f; std::atomic<bool> good{true}; std::array<std::thread, 4> workers;
    for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
        const auto buffer = f.output + i * page;
        for (unsigned n = 0; n < 12; ++n) {
            if (resize((2 + 2 * (n % 2)) * mib).rc != k::kOk || call(6, buffer, 0, heap).rc != k::kOk) good = false;
            Info info{}; std::memcpy(&info, reinterpret_cast<void*>(buffer), sizeof info);
            if (info.base != heap || info.state != mem::kMemHeap || info.perm != 3 || !info.size || info.size > 4 * mib) good = false;
        }
    });
    for (auto& worker : workers) worker.join(); CHECK(good.load());
    CHECK_EQ(mem::heap_committed(), nxrt::svc_current_heap_size());
}
NXTEST(memory_svc_reset_drops_old_heap_and_permission_locks) {
    Fixture f; f.module(); REQUIRE(resize(2 * mib).rc == k::kOk);
    REQUIRE(attribute(image, page, 16, 16).rc == k::kOk);
    nxrt::svc_memory_reset_for_tests(); mem::shutdown();
    REQUIRE(mem::init({.heap_reserve = 64 * mib})); f.output = mem::alloc_kernel(page); REQUIRE(f.output); f.module();
    CHECK_EQ(nxrt::svc_current_heap_size(), 0u); CHECK_EQ(f.query(image).attr, 0u);
    CHECK_EQ(permission(image, page, 1).rc, k::kOk); CHECK_EQ(resize(2 * mib).rc, k::kOk);
}
NXTEST(lifted_arm_program_sizes_queries_and_protects_the_guest_heap) {
    Fixture f;
    for (int i = 0; i < nxmod_svc_fixture.nfns; ++i)
        nxrt::fn_register(nxmod_svc_fixture.fns[i].addr, nxmod_svc_fixture.fns[i].fn);
    std::memset(reinterpret_cast<void*>(f.output), 0xcd, 128);
    ::Ctx c{}; c.x[0] = f.output;
    nxrt::dispatch_call(&c, 0x7300000400);
    for (unsigned offset : {0, 4, 8, 12, 16}) CHECK_EQ(rd32(f.output + offset), 0u);
    CHECK_EQ(rd64(f.output + 24), 0x55aau); CHECK_EQ(rd64(f.output + 32), heap);
    CHECK_EQ(rd64(f.output + 40), page); CHECK_EQ(rd32(f.output + 48), mem::kMemHeap);
    CHECK_EQ(rd32(f.output + 56), 1u); CHECK_EQ(nxrt::svc_current_heap_size(), 0u);
    CHECK_EQ(nxrt::svc_unknown_total(), 0u);
}
