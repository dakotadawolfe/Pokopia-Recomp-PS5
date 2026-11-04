// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/shared_heap.h"
#include "nxrt/guest_region.h"
#include "nxrecomp/ctx.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdlib>

NXTEST_GROUP(shared_heap)
namespace {
using nxrt::mem::SharedHeap;
using nxrt::mem::Prot;
constexpr uint64_t base = nxrt::mem::kHeapBase, alias_base = nxrt::mem::kAliasBase;
constexpr uint64_t page = 4096, granule = 65536, block = 2 * 1024 * 1024;
std::atomic<unsigned> fail_map{0}, fail_protect{0};
bool fail_once(std::atomic<unsigned>& countdown) {
    unsigned value = countdown.load();
    while (value && !countdown.compare_exchange_weak(value, value - 1)) {}
    return value == 1;
}
struct Inject {
    Inject(unsigned maps, unsigned protects) { fail_map = maps; fail_protect = protects; }
    ~Inject() { fail_map = 0; fail_protect = 0; }
};
sigjmp_buf fault_return;
volatile sig_atomic_t expected_fault = 0;
void on_fault(int signal) { if (expected_fault) siglongjmp(fault_return, 1); _Exit(128 + signal); }
struct FaultScope {
    struct sigaction old_segv{}, old_bus{};
    bool segv, bus;
    FaultScope() {
        struct sigaction action{}; action.sa_handler = on_fault; sigemptyset(&action.sa_mask);
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
    if (write) wr32(address, 0x1234abcd); else { volatile auto value = rd32(address); (void)value; }
    expected_fault = 0; return true;
}
}

// Only this test executable wraps these calls. Successful operations use the
// real native APIs; a selected call fails without changing its mapping.
extern "C" int __real_sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
extern "C" int __wrap_sceKernelMapDirectMemory(void** at, size_t size, int prot, int flags, int64_t physical, size_t alignment) {
    if (fail_once(fail_map)) return static_cast<int>(0x8002000cu);
    return __real_sceKernelMapDirectMemory(at, size, prot, flags, physical, alignment);
}
extern "C" int __real_mprotect(void*, size_t, int);
extern "C" int __wrap_mprotect(void* address, size_t size, int protection) {
    if (fail_once(fail_protect)) { errno = ENOMEM; return -1; }
    return __real_mprotect(address, size, protection);
}

NXTEST(heap_reserves_without_backing_and_counts_unique_pages) {
    CHECK(!SharedHeap::create(0)); CHECK(!SharedHeap::create(nxrt::mem::kHeapMax + 1));
    auto heap = SharedHeap::create(3 * block + 1);
    REQUIRE(heap);
    CHECK_EQ(heap->reserved_bytes(), 3 * block + granule);
    CHECK_EQ(heap->backed_bytes(), uint64_t{0});
    CHECK(!SharedHeap::create(block));
    REQUIRE(heap->commit(17, page));
    CHECK_EQ(heap->committed_bytes(), 2 * page);
    for (uint64_t offset = 0; offset < 2 * page; offset += 8) CHECK_EQ(rd64(base + offset), uint64_t{0});
    wr32(base, 71);
    REQUIRE(heap->commit(0, page));
    CHECK_EQ(heap->committed_bytes(), 2 * page);
    CHECK_EQ(rd32(base), uint32_t{71});
    CHECK_EQ(heap->backed_bytes(), block);
}

NXTEST(alias_created_before_commit_observes_new_pages) {
    auto heap = SharedHeap::create(4 * block);
    FaultScope faults; REQUIRE(heap && faults.segv && faults.bus);
    const auto a = heap->alias(granule, granule);
    REQUIRE(a);
    CHECK(!touch(a, false)); CHECK(!heap->is_mapped(a, page));
    REQUIRE(heap->commit(granule, page));
    wr32(base + granule, 123);
    CHECK_EQ(rd32(a), uint32_t{123});
    wr32(a, 456); CHECK_EQ(rd32(base + granule), uint32_t{456});
    CHECK(!touch(a + page, false));
    REQUIRE(heap->commit(granule + page, page));
    CHECK_EQ(rd32(a + page), uint32_t{0});
    CHECK_EQ(rd32(a), uint32_t{456});
}

NXTEST(alias_commit_publishes_section_pages_without_primary_counter) {
    auto heap = SharedHeap::create(block); REQUIRE(heap);
    const auto a = heap->alias(0, granule); REQUIRE(a);
    REQUIRE(heap->commit_at(a, page, Prot::RW));
    wr32(a, 0xcafe1234);
    CHECK_EQ(rd32(base), uint32_t{0xcafe1234});
    CHECK(heap->is_mapped(base, page));
    CHECK_EQ(heap->committed_bytes(), uint64_t{0});
    REQUIRE(heap->commit(0, page));
    CHECK_EQ(heap->committed_bytes(), page);
    CHECK_EQ(rd32(a), uint32_t{0xcafe1234});
}

NXTEST(primary_decommit_retains_alias_contents_and_recommit_data) {
    auto heap = SharedHeap::create(block); FaultScope faults;
    REQUIRE(heap && faults.segv && faults.bus);
    REQUIRE(heap->commit(0, 2 * page));
    const auto a = heap->alias(0, granule); REQUIRE(a);
    wr32(base, 100); wr32(base + page, 200);
    REQUIRE(heap->decommit(0, page));
    CHECK(!touch(base, false)); CHECK_EQ(rd32(a), uint32_t{100});
    CHECK_EQ(rd32(base + page), uint32_t{200});
    CHECK_EQ(heap->committed_bytes(), page);
    wr32(a, 300);
    REQUIRE(heap->commit(0, page));
    CHECK_EQ(rd32(base), uint32_t{300});
    REQUIRE(heap->decommit(0, block));
    CHECK_EQ(heap->committed_bytes(), uint64_t{0});
    CHECK_EQ(heap->backed_bytes(), block);
    CHECK_EQ(rd32(a), uint32_t{300});
    const auto late = heap->alias(0, granule); REQUIRE(late);
    CHECK_EQ(rd32(late), uint32_t{300});
}

NXTEST(alias_permissions_are_independent_and_preserve_neighbors) {
    auto heap = SharedHeap::create(block); FaultScope faults;
    REQUIRE(heap && faults.segv && faults.bus);
    const auto a = heap->alias(0, granule, 0, Prot::R);
    const auto b = heap->alias(0, granule, 0, Prot::None); REQUIRE(a && b);
    REQUIRE(heap->commit(0, 2 * page));
    CHECK(touch(base, true)); CHECK(touch(a, false)); CHECK(!touch(a, true)); CHECK(!touch(b, false));
    REQUIRE(heap->protect(a + page, page, Prot::RW));
    CHECK(touch(a + page, true)); CHECK(!touch(a, true));
    CHECK(!heap->protect(a, 3 * page, Prot::RW));
    CHECK(!touch(a, true));
    REQUIRE(heap->protect(b, page, Prot::R));
    CHECK_EQ(rd32(b), rd32(base));
}

NXTEST(cross_block_alias_uses_the_correct_section_offsets) {
    auto heap = SharedHeap::create(4 * block); REQUIRE(heap);
    const auto a = heap->alias(block - granule, 2 * granule); REQUIRE(a);
    REQUIRE(heap->commit(block - page, 2 * page));
    wr32(base + block - page, 37); wr32(base + block, 83);
    CHECK_EQ(rd32(a + granule - page), uint32_t{37});
    CHECK_EQ(rd32(a + granule), uint32_t{83});
    CHECK_EQ(heap->backed_bytes(), 2 * block);
    uint64_t offset = 0;
    CHECK(heap->offset_of(a + granule + 7, &offset)); CHECK_EQ(offset, block + 7);
    CHECK(!heap->offset_of(a + 2 * granule, &offset));
}

NXTEST(private_views_share_later_commits_and_survive_primary_decommit) {
    auto heap = SharedHeap::create(3 * block); REQUIRE(heap);
    auto* raw = static_cast<uint32_t*>(heap->private_view(granule, page + 1)); REQUIRE(raw);
    REQUIRE(heap->commit(granule, 2 * page));
    CHECK_EQ(raw[0], uint32_t{0});
    raw[0] = 19; CHECK_EQ(rd32(base + granule), uint32_t{19});
    wr32(base + granule + page, 27); CHECK_EQ(raw[page / 4], uint32_t{27});
    REQUIRE(heap->decommit(granule, 2 * page));
    CHECK_EQ(raw[0], uint32_t{19});
    CHECK_EQ(heap->alias_count(), size_t{0});
    uint64_t offset = 0; CHECK(!heap->offset_of(reinterpret_cast<uint64_t>(raw), &offset));
    CHECK(heap->unmap_private(raw)); CHECK(!heap->unmap_private(raw));
}

NXTEST(unmapping_one_view_keeps_primary_and_other_aliases) {
    auto heap = SharedHeap::create(block); FaultScope faults;
    REQUIRE(heap && faults.segv && faults.bus);
    REQUIRE(heap->commit(0, page)); wr32(base, 41);
    const auto a = heap->alias(0, granule), b = heap->alias(0, granule); REQUIRE(a && b && a != b);
    CHECK_EQ(heap->alias_count(), size_t{2});
    CHECK(heap->unmap_alias(a)); CHECK(!heap->unmap_alias(a));
    CHECK(!touch(a, false)); CHECK_EQ(rd32(b), uint32_t{41});
    CHECK_EQ(rd32(base), uint32_t{41}); CHECK_EQ(heap->alias_count(), size_t{1});
    const auto reused = heap->alias(0, granule); CHECK_EQ(reused, a);
    std::vector<std::pair<uint64_t, uint64_t>> ranges; heap->alias_ranges(ranges);
    CHECK_EQ(ranges.size(), size_t{2});
    for (const auto& range : ranges) { CHECK_EQ(range.first, uint64_t{0}); CHECK_EQ(range.second, granule); }
}

NXTEST(alias_selection_and_explicit_address_do_not_overwrite_other_owner) {
    auto occupied = nxrt::mem::GuestRegion::create(alias_base, granule); REQUIRE(occupied);
    REQUIRE(occupied->commit(alias_base, page)); wr32(alias_base, 99);
    auto heap = SharedHeap::create(block); REQUIRE(heap);
    CHECK_EQ(heap->alias(0, granule, alias_base), uint64_t{0});
    const auto a = heap->alias(0, granule); REQUIRE(a);
    CHECK(a >= alias_base + granule); CHECK_EQ(rd32(alias_base), uint32_t{99});
}

NXTEST(invalid_heap_operations_leave_existing_data_and_views_intact) {
    auto heap = SharedHeap::create(block); REQUIRE(heap);
    REQUIRE(heap->commit(0, page)); wr32(base, 55);
    CHECK(!heap->commit(block, page)); CHECK(!heap->commit(UINT64_MAX, page));
    CHECK(!heap->decommit(0, UINT64_MAX));
    CHECK(!heap->protect(base, page, static_cast<Prot>(42)));
    CHECK_EQ(heap->alias(page, granule), uint64_t{0}); CHECK_EQ(heap->alias(0, page), uint64_t{0});
    CHECK_EQ(heap->alias(0, granule, alias_base + page), uint64_t{0});
    CHECK(!heap->private_view(page, page)); CHECK(!heap->private_view(0, UINT64_MAX));
    CHECK(!heap->commit_at(nxrt::mem::kMainBase, page, Prot::RW));
    CHECK_EQ(rd32(base), uint32_t{55}); CHECK_EQ(heap->committed_bytes(), page);
    CHECK(heap->commit(0, 0)); CHECK(heap->decommit(0, 0));
}

NXTEST(native_mapping_failure_rolls_back_all_new_bindings) {
    auto heap = SharedHeap::create(2 * block); REQUIRE(heap);
    const auto a = heap->alias(0, granule); REQUIRE(a);
    { Inject failure(3, 0); CHECK(!heap->commit(0, page)); CHECK_EQ(fail_map.load(), 0u); }
    CHECK_EQ(heap->backed_bytes(), uint64_t{0}); CHECK_EQ(heap->committed_bytes(), uint64_t{0});
    CHECK(!heap->is_mapped(base, page)); CHECK(!heap->is_mapped(a, page));
    REQUIRE(heap->commit(0, page));
    CHECK_EQ(rd32(base), uint32_t{0}); wr32(a, 17); CHECK_EQ(rd32(base), uint32_t{17});
}

NXTEST(native_protection_failure_restores_preexisting_permissions_and_data) {
    auto heap = SharedHeap::create(2 * block); FaultScope faults;
    REQUIRE(heap && faults.segv && faults.bus);
    const auto a = heap->alias(0, granule, 0, Prot::R); REQUIRE(a);
    REQUIRE(heap->commit(0, page)); wr32(base, 71);
    { Inject failure(0, 2); CHECK(!heap->commit(16384, page)); CHECK_EQ(fail_protect.load(), 0u); }
    CHECK_EQ(heap->committed_bytes(), page);
    CHECK(!touch(base + 16384, false)); CHECK(!touch(a + 16384, false));
    CHECK_EQ(rd32(base), uint32_t{71}); CHECK_EQ(rd32(a), uint32_t{71}); CHECK(!touch(a, true));
    REQUIRE(heap->commit(16384, page));
    CHECK_EQ(rd32(a + 16384), uint32_t{0});
}

NXTEST(heap_reset_releases_all_views_and_allows_clean_recreation) {
    auto heap = SharedHeap::create(block); FaultScope faults;
    REQUIRE(heap && faults.segv && faults.bus);
    REQUIRE(heap->commit(0, page));
    const auto a = heap->alias(0, granule); REQUIRE(a);
    void* p = heap->private_view(0, page); REQUIRE(p);
    REQUIRE(heap->reset()); CHECK(heap->reset());
    CHECK(!touch(base, false)); CHECK(!touch(a, false));
    CHECK_EQ(heap->reserved_bytes(), uint64_t{0}); CHECK_EQ(heap->backed_bytes(), uint64_t{0});
    CHECK(!heap->commit(0, page)); CHECK(!heap->unmap_private(p));
    auto fresh = SharedHeap::create(block); REQUIRE(fresh);
    REQUIRE(fresh->commit(0, page)); CHECK_EQ(rd32(base), uint32_t{0});
}
