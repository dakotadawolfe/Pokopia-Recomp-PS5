// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/guest_region.h"
#include "nxrecomp/ctx.h"
#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <limits>

NXTEST_GROUP(guest_region)
namespace {
using nxrt::mem::GuestRegion;
using nxrt::mem::Prot;
constexpr uint64_t base = nxrt::mem::kMainBase, page = 4096, block = 2 * 1024 * 1024;
sigjmp_buf fault_return;
volatile sig_atomic_t expected_fault = 0;
void on_fault(int signal) {
    if (expected_fault) siglongjmp(fault_return, 1);
    _Exit(128 + signal);
}
struct FaultScope {
    struct sigaction old_segv{}, old_bus{};
    bool segv = false, bus = false;
    FaultScope() {
        struct sigaction action{};
        action.sa_handler = on_fault;
        sigemptyset(&action.sa_mask);
        segv = sigaction(SIGSEGV, &action, &old_segv) == 0;
        bus = sigaction(SIGBUS, &action, &old_bus) == 0;
    }
    ~FaultScope() {
        if (segv) sigaction(SIGSEGV, &old_segv, nullptr);
        if (bus) sigaction(SIGBUS, &old_bus, nullptr);
    }
};
bool touch(uint64_t address, bool write) {
    if (sigsetjmp(fault_return, 1)) { expected_fault = 0; return false; }
    expected_fault = 1;
    if (write) wr32(address, 0x56789abc);
    else { volatile auto value = rd32(address); (void)value; }
    expected_fault = 0;
    return true;
}
}

NXTEST(reservation_bounds_and_occupied_ranges_preserve_owner) {
    CHECK(!GuestRegion::create(0, page));
    CHECK(!GuestRegion::create(base + page, page));
    CHECK(!GuestRegion::create(base, 0));
    CHECK(!GuestRegion::create(base, UINT64_MAX));
    CHECK(!GuestRegion::create(NX_GUEST_ADDRESS_END, page));
    auto region = GuestRegion::create(base, page + 1);
    REQUIRE(region);
    CHECK_EQ(region->backed_bytes(), uint64_t{0});
    CHECK(!GuestRegion::create(base, 16384));
    REQUIRE(region->commit(base, 2 * page));
    wr32(base, 123);
    CHECK(!region->commit(base + 2 * page, page));
    CHECK(!region->protect(base, 3 * page, Prot::R));
    CHECK(!region->decommit(base - page, page));
    CHECK(!region->commit(base, UINT64_MAX));
    CHECK(!region->commit(base, page, static_cast<Prot>(42)));
    CHECK_EQ(rd32(base), uint32_t{123});
    CHECK_EQ(region->committed_bytes(), 2 * page);
    CHECK_EQ(region->backed_bytes(), uint64_t{16384});
}

NXTEST(new_pages_are_zero_and_recommit_preserves_existing_contents) {
    auto region = GuestRegion::create(base, block);
    REQUIRE(region);
    REQUIRE(region->commit(base + 17, page)); // Both intersecting guest pages.
    CHECK_EQ(region->committed_bytes(), 2 * page);
    for (uint64_t offset = 0; offset < 2 * page; offset += 8) CHECK_EQ(rd64(base + offset), uint64_t{0});
    wr32(base, 0x12345678);
    REQUIRE(region->commit(base, page, Prot::RX));
    CHECK_EQ(rd32(base), uint32_t{0x12345678});
    CHECK_EQ(region->committed_bytes(), 2 * page);
    CHECK(region->is_mapped(base, 2 * page));
    CHECK(!region->is_mapped(base, 3 * page));
    CHECK(!region->protect(base, 3 * page, Prot::RW));
    CHECK(!nxrt::mem::SubpageMap::allows(base, page, 2));
    CHECK(nxrt::mem::SubpageMap::allows(base + page, page, 2));
}

NXTEST(mixed_permissions_and_reserved_tail_fault_with_checked_accesses) {
    auto region = GuestRegion::create(base, 3 * page);
    FaultScope faults;
    REQUIRE(region && faults.segv && faults.bus);
    REQUIRE(region->commit(base, 3 * page));
    REQUIRE(region->protect(base, page, Prot::R));
    REQUIRE(region->protect(base + page, page, Prot::None));
    CHECK(touch(base, false));
    CHECK(!touch(base, true));
    CHECK(!touch(base + page, false));
    CHECK(touch(base + 2 * page, true));
    CHECK(!touch(base + 3 * page, false)); // Host-rounded padding is never committed.
    CHECK(!region->is_mapped(base, 3 * page));
    REQUIRE(region->protect(base + page, page, Prot::RWX));
    CHECK_EQ(rd32(base + page), uint32_t{0});
    CHECK(region->is_mapped(base, 3 * page));
}

NXTEST(decommit_zeros_one_guest_page_and_keeps_neighbor) {
    auto region = GuestRegion::create(base, block);
    FaultScope faults;
    REQUIRE(region && faults.segv && faults.bus);
    REQUIRE(region->commit(base, 2 * page));
    wr32(base, 101); wr32(base + page, 202);
    REQUIRE(region->decommit(base + 10, 1));
    CHECK_EQ(region->committed_bytes(), page);
    CHECK(!touch(base, false));
    CHECK_EQ(rd32(base + page), uint32_t{202});
    CHECK(!region->protect(base, page, Prot::RW));
    REQUIRE(region->commit(base, page));
    for (uint64_t offset = 0; offset < page; offset += 8) CHECK_EQ(rd64(base + offset), uint64_t{0});
    CHECK_EQ(rd32(base + page), uint32_t{202});
    REQUIRE(region->decommit(base, block));
    CHECK_EQ(region->committed_bytes(), uint64_t{0});
    CHECK_EQ(region->backed_bytes(), uint64_t{0});
    CHECK(!touch(base, false));
    REQUIRE(region->commit(base, page));
    CHECK_EQ(rd64(base), uint64_t{0});
}

NXTEST(cross_block_changes_preserve_committed_counts_and_contents) {
    auto region = GuestRegion::create(base, 3 * block);
    REQUIRE(region);
    REQUIRE(region->commit(base + block - page, 2 * page, Prot::None));
    CHECK_EQ(region->committed_bytes(), 2 * page);
    CHECK_EQ(region->backed_bytes(), 2 * block);
    CHECK(!region->is_mapped(base + block - page, 2 * page));
    REQUIRE(region->protect(base + block - page, 2 * page, Prot::RW));
    wr32(base + block - page, 17); wr32(base + block, 29);
    CHECK(!region->commit(base + 3 * block - page, 2 * page));
    CHECK_EQ(rd32(base + block - page), uint32_t{17});
    CHECK_EQ(rd32(base + block), uint32_t{29});
    REQUIRE(region->decommit(base + block, block));
    CHECK_EQ(region->committed_bytes(), page);
    CHECK_EQ(region->backed_bytes(), block);
    CHECK_EQ(rd32(base + block - page), uint32_t{17});
    REQUIRE(region->decommit(base, 3 * block));
    CHECK_EQ(region->backed_bytes(), uint64_t{0});
}

NXTEST(four_gib_region_commits_only_the_touched_blocks) {
    constexpr uint64_t kernel = nxrt::mem::kKernelBase, size = nxrt::mem::kKernelSize;
    auto region = GuestRegion::create(kernel, size);
    REQUIRE(region);
    REQUIRE(region->commit(kernel, page));
    REQUIRE(region->commit(kernel + size - page, page));
    wr32(kernel, 19); wr32(kernel + size - page, 83);
    CHECK_EQ(region->backed_bytes(), 2 * block);
    CHECK_EQ(region->committed_bytes(), 2 * page);
    CHECK_EQ(rd32(kernel), uint32_t{19});
    CHECK_EQ(rd32(kernel + size - page), uint32_t{83});
    CHECK(!region->is_mapped(kernel + block, page));
    REQUIRE(region->decommit(kernel, size));
    CHECK_EQ(region->backed_bytes(), uint64_t{0});
    CHECK_EQ(region->committed_bytes(), uint64_t{0});
}

NXTEST(release_allows_reuse_and_zero_length_operations_change_nothing) {
    auto region = GuestRegion::create(base, page);
    REQUIRE(region);
    CHECK(region->commit(base, 0));
    CHECK(region->decommit(base, 0));
    CHECK(region->protect(base, 0, Prot::R));
    CHECK_EQ(region->backed_bytes(), uint64_t{0});
    REQUIRE(region->commit(base, page));
    wr32(base, 77);
    CHECK(region->reset());
    CHECK(region->reset());
    CHECK(!region->commit(base, page));
    CHECK_EQ(nx_guest_subpage_flags[base >> 14], uint8_t{0});
    auto replacement = GuestRegion::create(base, page);
    REQUIRE(replacement);
    REQUIRE(replacement->commit(base, page));
    CHECK_EQ(rd32(base), uint32_t{0});
}
