// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/memory.h"
#include "nxrt/guest_region.h"
#include "nxrt/subpage.h"
#include "nxrecomp/ctx.h"
#include "protect_fault.h"
#include <atomic>
#include <cerrno>
#include <mutex>
#include <set>
#include <thread>

NXTEST_GROUP(memory_ps5)
namespace {
namespace mem = nxrt::mem;
constexpr uint64_t base = mem::kMainBase, granule = mem::kAllocGranularity;
struct Reset { Reset() { mem::shutdown(); } ~Reset() { test_fail_protect = 0; mem::shutdown(); } };
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
