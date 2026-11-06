// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/memory.h"
#include "nxrt/guest_region.h"
#include "nxrt/subpage.h"
#include "nxrt/write_watch.h"
#include "nxrecomp/ctx.h"
#include "protect_fault.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

NXTEST_GROUP(memory_ps5)
namespace {
namespace mem = nxrt::mem;
constexpr uint64_t base = mem::kMainBase, granule = mem::kAllocGranularity;
struct Reset { Reset() { mem::shutdown(); } ~Reset() { test_fail_protect = 0; mem::shutdown(); } };
}

NXTEST(snapshot_copies_readable_pages_and_rejects_entire_invalid_span) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::reserve_fixed(base, granule, "snapshot"));
    REQUIRE(mem::commit(base, granule));
    std::vector<uint8_t> input(granule, 0x35), output(granule, 0xa7);
    std::memcpy(reinterpret_cast<void*>(base), input.data(), input.size());
    REQUIRE(mem::copy_readable(base, output.data(), output.size())); CHECK(output == input);
    REQUIRE(mem::protect(base + mem::kPage, mem::kPage, mem::Prot::None));
    output.assign(granule, 0xa7);
    CHECK(!mem::copy_readable(base, output.data(), output.size()));
    CHECK(output == std::vector<uint8_t>(granule, 0xa7));
    CHECK(!mem::copy_readable(UINT64_MAX - 4, output.data(), 16));
    CHECK(!mem::copy_readable(base, nullptr, 1));
    CHECK(mem::copy_readable(0, nullptr, 0));
    REQUIRE(mem::release_fixed(base));
    CHECK(!mem::copy_readable(base, output.data(), mem::kPage));
}

NXTEST(gpu_import_views_reject_holes_and_preserve_primary_policy) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::heap_create(2 * granule));
    CHECK(mem::heap_import_view(0, granule) == nullptr);
    REQUIRE(mem::heap_commit(0, mem::kPage));
    CHECK(mem::heap_import_view(0, granule) == nullptr);
    REQUIRE(mem::heap_commit(0, granule)); wr32(mem::kHeapBase, 0x12345678);
    REQUIRE(mem::protect(mem::kHeapBase, granule, mem::Prot::R));
    void* imported = mem::heap_import_view(0, granule); REQUIRE(imported);
    CHECK_EQ(*static_cast<uint32_t*>(imported), uint32_t{0x12345678});
    *static_cast<uint32_t*>(imported) = 0x9abc;
    CHECK_EQ(rd32(mem::kHeapBase), uint32_t{0x9abc});
    CHECK(mem::SubpageMap::allows(mem::kHeapBase, granule, 1));
    CHECK(!mem::SubpageMap::allows(mem::kHeapBase, granule, 3));
    CHECK(mem::heap_private_view_unmap(imported));
    CHECK(mem::heap_import_view(0, granule + mem::kPage) == nullptr);
    CHECK(mem::heap_import_view(0, 2 * granule) == nullptr);
    CHECK(mem::heap_import_view(0, UINT64_MAX) == nullptr);
}

NXTEST(snapshot_remains_owned_after_alias_unmap_and_heap_decommit) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::heap_create(granule)); REQUIRE(mem::heap_commit(0, granule));
    const auto view = mem::heap_alias(0, granule); REQUIRE(view);
    std::memset(reinterpret_cast<void*>(view), 0x6b, granule);
    std::vector<uint8_t> copy(granule);
    REQUIRE(mem::copy_readable(view, copy.data(), copy.size()));
    REQUIRE(mem::heap_alias_unmap(view)); REQUIRE(mem::heap_decommit(0, granule));
    CHECK(copy == std::vector<uint8_t>(granule, 0x6b));
    CHECK(!mem::copy_readable(view, copy.data(), copy.size()));
}

NXTEST(snapshot_serializes_with_repeated_mapping_changes) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::reserve_fixed(base, granule, "snapshot-race"));
    REQUIRE(mem::commit(base, granule));
    std::memset(reinterpret_cast<void*>(base), 0x5c, granule);
    std::atomic<bool> stop{false}; std::atomic<unsigned> reads{0}, bad{0};
    std::thread reader([&] {
        std::vector<uint8_t> copy(granule);
        while (!stop.load()) {
            if (mem::copy_readable(base, copy.data(), copy.size()) && copy != std::vector<uint8_t>(granule, 0x5c)) ++bad;
            ++reads;
        }
    });
    while (!reads.load()) std::this_thread::yield();
    for (unsigned n = 0; n < 100; ++n) {
        CHECK(mem::protect(base, granule, mem::Prot::None));
        CHECK(mem::protect(base, granule, mem::Prot::RW));
    }
    stop.store(true); reader.join(); CHECK(reads.load() > 0); CHECK_EQ(bad.load(), 0u);
}

NXTEST(texture_tracker_detects_primary_and_alias_writes_without_page_guards) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::heap_create(granule)); REQUIRE(mem::heap_commit(0, granule));
    const auto alias = mem::heap_alias(0, granule); REQUIRE(alias);
    auto* region = mem::ww::region_create(mem::kHeapBase, granule); REQUIRE(region);
    struct RegionOwner { mem::ww::Region* r; ~RegionOwner() { mem::ww::region_destroy(r); } } owner{region};
    REQUIRE(mem::ww::arm(region, 0, 2));
    CHECK(mem::ww::all_valid(region, 0, 2));
    CHECK(!mem::ww::guard(region, 0, 1)); CHECK(!mem::ww::all_armed(region, 0, 1));
    wr32(mem::kHeapBase + 8, 0x1234); wr32(alias + mem::kPage + 32, 0x5678);
    CHECK(!mem::ww::page_valid(region, 0)); CHECK(!mem::ww::page_valid(region, 1));
    std::vector<uint64_t> changed; mem::ww::take_written(region, changed);
    CHECK(changed == std::vector<uint64_t>({0, 1}));
    changed.clear(); mem::ww::take_written(region, changed); CHECK(changed.empty());
    REQUIRE(mem::ww::arm(region, 0, 2)); CHECK(mem::ww::all_valid(region, 0, 2));
    REQUIRE(mem::heap_alias_unmap(alias)); REQUIRE(mem::heap_decommit(0, granule));
    mem::ww::take_written(region, changed); CHECK(changed == std::vector<uint64_t>({0, 1}));
}

NXTEST(texture_tracker_failed_arm_preserves_prior_snapshot) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::heap_create(granule)); REQUIRE(mem::heap_commit(0, granule));
    auto* region = mem::ww::region_create(mem::kHeapBase, granule); REQUIRE(region);
    struct RegionOwner { mem::ww::Region* r; ~RegionOwner() { mem::ww::region_destroy(r); } } owner{region};
    REQUIRE(mem::ww::arm(region, 0, 1)); wr32(mem::kHeapBase, 9);
    REQUIRE(mem::protect(mem::kHeapBase + mem::kPage, mem::kPage, mem::Prot::None));
    CHECK(!mem::ww::arm(region, 0, 2));
    std::vector<uint64_t> changed; mem::ww::take_written(region, changed);
    CHECK(changed == std::vector<uint64_t>({0}));
    CHECK(!mem::ww::arm(region, UINT64_MAX, 1));
}

NXTEST(texture_tracker_range_checks_preserve_changes_for_unused_textures) {
    Reset reset;
    REQUIRE(mem::init()); REQUIRE(mem::heap_create(granule)); REQUIRE(mem::heap_commit(0, granule));
    const auto alias = mem::heap_alias(0, granule); REQUIRE(alias);
    auto* region = mem::ww::region_create(mem::kHeapBase, granule); REQUIRE(region);
    struct RegionOwner { mem::ww::Region* r; ~RegionOwner() { mem::ww::region_destroy(r); } } owner{region};
    REQUIRE(mem::ww::arm(region, 0, 8));
    wr32(mem::kHeapBase + 4096 + 20, 21);
    wr32(alias + 6 * 4096 + 12, 31);
    std::vector<uint64_t> changed;
    mem::ww::take_written_range(region, 2, 3, changed); CHECK(changed.empty());
    mem::ww::take_written_range(region, 0, 2, changed); CHECK(changed == std::vector<uint64_t>({1}));
    changed.clear();
    mem::ww::take_written_range(region, 1, 2, changed); CHECK(changed.empty());
    mem::ww::invalidate(region, 4, 1);
    mem::ww::before_os_write(reinterpret_cast<void*>(mem::kHeapBase + 7 * 4096), 1);
    mem::ww::take_written_range(region, UINT64_MAX, 2, changed); CHECK(changed.empty());
    mem::ww::take_written_range(region, 2, 0, changed); CHECK(changed.empty());
    mem::ww::take_written_range(region, 5, 3, changed); CHECK(changed == std::vector<uint64_t>({6, 7}));
    changed.clear();
    mem::ww::take_written(region, changed); CHECK(changed == std::vector<uint64_t>({4}));
    changed.clear();
    REQUIRE(mem::heap_alias_unmap(alias)); REQUIRE(mem::heap_decommit(0, granule));
    mem::ww::take_written_range(region, 2, 2, changed); CHECK(changed == std::vector<uint64_t>({2, 3}));
}

NXTEST(failed_init_keeps_foreign_reservation_and_releases_its_stack) {
    Reset reset;
    auto occupied = mem::GuestRegion::create(mem::kKernelBase, granule); REQUIRE(occupied);
    REQUIRE(occupied->commit(mem::kKernelBase, mem::kPage)); wr32(mem::kKernelBase, 73);
    CHECK(!mem::init()); CHECK(!mem::initialised());
    CHECK_EQ(rd32(mem::kKernelBase), uint32_t{73});
    REQUIRE(mem::reserve_fixed(mem::kStackBase, granule, "init rollback"));
    CHECK(mem::release_fixed(mem::kStackBase));
    occupied.reset(); CHECK(mem::init());
}

NXTEST(cross_reservation_protect_preflights_and_rolls_back_native_failure) {
    Reset reset;
    REQUIRE(mem::reserve_fixed(base, granule, "first"));
    REQUIRE(mem::reserve_fixed(base + granule, granule, "second"));
    REQUIRE(mem::commit(base, granule)); wr32(base, 73);
    CHECK(!mem::protect(base, 2 * granule, mem::Prot::None));
    CHECK(mem::SubpageMap::allows(base, granule, 3)); CHECK_EQ(rd32(base), uint32_t{73});
    REQUIRE(mem::commit(base + granule, granule)); wr32(base + granule, 84);
    REQUIRE(mem::protect(base, granule, mem::Prot::R));
    test_fail_protect = 2;
    CHECK(!mem::protect(base, 2 * granule, mem::Prot::None));
    CHECK_EQ(test_fail_protect.load(), 0u);
    CHECK(mem::SubpageMap::allows(base, granule, 1)); CHECK(!mem::SubpageMap::allows(base, granule, 3));
    CHECK(mem::SubpageMap::allows(base + granule, granule, 3));
    CHECK_EQ(rd32(base), uint32_t{73}); CHECK_EQ(rd32(base + granule), uint32_t{84});
    CHECK(mem::is_mapped(base, 2 * granule));
    REQUIRE(mem::protect(base, 2 * granule, mem::Prot::None)); CHECK(!mem::is_mapped(base, 2 * granule));
    REQUIRE(mem::protect(base, 2 * granule, mem::Prot::RW)); CHECK(mem::is_mapped(base, 2 * granule));
}

NXTEST(concurrent_kernel_allocations_are_disjoint_and_keep_data) {
    Reset reset;
    REQUIRE(mem::init());
    std::mutex lock;
    std::set<uint64_t> addresses;
    std::atomic<bool> good{true};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&] {
        for (unsigned i = 0; i < 64; ++i) {
            const auto address = mem::alloc_kernel(mem::kPage, granule);
            if (!address || address % granule) { good = false; return; }
            if (rd32(address)) good = false;
            wr64(address, address ^ 0x1234);
            std::lock_guard held(lock);
            if (!addresses.insert(address).second) good = false;
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(good.load()); CHECK_EQ(addresses.size(), size_t{256});
    for (const auto address : addresses) CHECK_EQ(rd64(address), address ^ 0x1234);
}

NXTEST(memory_api_rejects_overflow_and_keeps_vm_bounds) {
    Reset reset;
    REQUIRE(mem::init());
    CHECK(!mem::reserve_fixed(base, UINT64_MAX, "overflow"));
    CHECK(!mem::commit(UINT64_MAX, 2)); CHECK(!mem::decommit(UINT64_MAX, 2));
    CHECK(!mem::protect(UINT64_MAX, 2, mem::Prot::RW)); CHECK(!mem::is_mapped(UINT64_MAX, 2));
    CHECK_EQ(mem::alloc_kernel(1, UINT64_MAX), uint64_t{0});
    CHECK(!mem::heap_create(UINT64_MAX));
    mem::vm_set(base, granule, mem::kMemCode, 5);
    mem::vm_set(base, UINT64_MAX, mem::kMemHeap, 3);
    CHECK_EQ(mem::vm_query(base).state, uint32_t{mem::kMemCode});
    CHECK_EQ(mem::vm_query(NX_GUEST_ADDRESS_END).state, uint32_t{mem::kMemInaccessible});
    CHECK_EQ(mem::vm_query(UINT64_MAX).size, uint64_t{0});
    CHECK_EQ(mem::vm_query(NX_GUEST_ADDRESS_END - 1).addr + mem::vm_query(NX_GUEST_ADDRESS_END - 1).size,
             uint64_t{NX_GUEST_ADDRESS_END});
}
