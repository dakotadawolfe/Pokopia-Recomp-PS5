// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/host_vm.h"
#include "nxrt/memory.h"
#ifdef NXRT_TEST_PS5
#include "nxrt/ps5_vm.h"
#endif
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <unistd.h>
#include <utility>

NXTEST_GROUP(host_vm)
namespace vm = nxrt::host_vm;
namespace {
sigjmp_buf fault_return;
volatile sig_atomic_t expected_fault = 0;
void on_fault(int signal) {
    if (expected_fault) siglongjmp(fault_return, 1);
    _Exit(128 + signal);
}
struct FaultScope {
    struct sigaction old_segv{}, old_bus{};
    bool segv_set = false, bus_set = false;
    FaultScope() {
        struct sigaction action{};
        action.sa_handler = on_fault;
        sigemptyset(&action.sa_mask);
        segv_set = sigaction(SIGSEGV, &action, &old_segv) == 0;
        bus_set = sigaction(SIGBUS, &action, &old_bus) == 0;
    }
    ~FaultScope() {
        if (segv_set) sigaction(SIGSEGV, &old_segv, nullptr);
        if (bus_set) sigaction(SIGBUS, &old_bus, nullptr);
    }
};
bool can_touch(void* memory, size_t offset, bool write) {
    if (sigsetjmp(fault_return, 1)) { expected_fault = 0; return false; }
    expected_fault = 1;
    auto* p = static_cast<volatile unsigned char*>(memory) + offset;
    if (write) *p = 0x5a;
    else { volatile auto value = *p; (void)value; }
    expected_fault = 0;
    return true;
}
unsigned char& byte(vm::Mapping& mapping, size_t offset = 0) {
    return static_cast<unsigned char*>(mapping.data())[offset];
}
}

NXTEST(host_pages_and_invalid_ranges) {
    const size_t page = vm::page_size();
    REQUIRE(page == static_cast<size_t>(sysconf(_SC_PAGESIZE)));
    REQUIRE(page >= 4096);
    CHECK(!vm::Mapping::reserve(0));
    CHECK(!vm::Mapping::reserve(page - 1));
    CHECK(!vm::Mapping::reserve(page, nxrt::mem::kMainBase + 1));
    CHECK(!vm::SharedObject::create(0));
    CHECK(!vm::SharedObject::create(page - 1));
    const auto top = std::numeric_limits<uintptr_t>::max() - page + 1;
    CHECK(!vm::Mapping::reserve(2 * page, top));
}

NXTEST(owned_mapping_refuses_overlap_and_releases) {
    const size_t page = vm::page_size();
    auto mapping = vm::Mapping::reserve(4 * page, nxrt::mem::kMainBase);
    REQUIRE(mapping);
    REQUIRE(mapping.protect(0, 4 * page, vm::Access::ReadWrite));
    byte(mapping) = 71;
    CHECK(!vm::Mapping::reserve(4 * page, nxrt::mem::kMainBase));
    CHECK(!vm::Mapping::reserve(4 * page, nxrt::mem::kMainBase + page));
    CHECK_EQ(byte(mapping), 71);
    CHECK(mapping.reset());
    CHECK(mapping.reset());
    CHECK(!mapping);
    auto reused = vm::Mapping::reserve(4 * page, nxrt::mem::kMainBase);
    REQUIRE(reused);
    REQUIRE(reused.protect(0, page, vm::Access::ReadWrite));
    CHECK_EQ(byte(reused), 0);
}

NXTEST(native_protection_faults_and_preserves_neighbor) {
    const size_t page = vm::page_size();
    FaultScope faults;
    REQUIRE(faults.segv_set && faults.bus_set);
    auto mapping = vm::Mapping::reserve(2 * page);
    REQUIRE(mapping);
    CHECK(!can_touch(mapping.data(), 0, false));
    REQUIRE(mapping.protect(0, 2 * page, vm::Access::ReadWrite));
    byte(mapping) = 31;
    REQUIRE(mapping.protect(0, page, vm::Access::Read));
    CHECK_EQ(byte(mapping), 31);
    CHECK(!can_touch(mapping.data(), 0, true));
    CHECK(can_touch(mapping.data(), page, true));
    REQUIRE(mapping.protect(0, page, vm::Access::None));
    CHECK(!can_touch(mapping.data(), 0, false));
    CHECK(can_touch(mapping.data(), page, true));
}

NXTEST(subpage_or_out_of_bounds_protection_has_no_side_effects) {
    const size_t page = vm::page_size();
    FaultScope faults;
    REQUIRE(faults.segv_set && faults.bus_set);
    auto mapping = vm::Mapping::reserve(2 * page);
    REQUIRE(mapping);
    REQUIRE(mapping.protect(0, 2 * page, vm::Access::ReadWrite));
    CHECK(!mapping.protect(0, page / 2, vm::Access::None));
    CHECK(!mapping.protect(page / 2, page, vm::Access::None));
    CHECK(!mapping.protect(page, 2 * page, vm::Access::None));
    CHECK(!mapping.protect(std::numeric_limits<size_t>::max(), page, vm::Access::None));
    CHECK(!mapping.protect(0, page, static_cast<vm::Access>(42)));
    CHECK(can_touch(mapping.data(), 0, true));
    CHECK(can_touch(mapping.data(), page, true));
}

NXTEST(shared_aliases_use_offsets_and_independent_permissions) {
    const size_t page = vm::page_size();
    FaultScope faults;
    REQUIRE(faults.segv_set && faults.bus_set);
    auto object = vm::SharedObject::create(4 * page);
    REQUIRE(object);
    auto primary = object.map(0, 4 * page, vm::Access::ReadWrite, nxrt::mem::kHeapBase);
    auto alias = object.map(page, 2 * page, vm::Access::ReadWrite, nxrt::mem::kAliasBase);
    REQUIRE(primary && alias);
    byte(primary, page) = 17;
    CHECK_EQ(byte(alias), 17);
    byte(alias, page) = 41;
    CHECK_EQ(byte(primary, 2 * page), 41);
    REQUIRE(primary.protect(page, page, vm::Access::None));
    CHECK(!can_touch(primary.data(), page, false));
    CHECK(can_touch(alias.data(), 0, true));
    REQUIRE(primary.protect(page, page, vm::Access::Read));
    CHECK_EQ(byte(primary, page), 0x5a);
    CHECK(!can_touch(primary.data(), page, true));
}

NXTEST(shared_views_refuse_bad_offsets_and_occupied_addresses) {
    const size_t page = vm::page_size();
    auto object = vm::SharedObject::create(2 * page);
    REQUIRE(object);
    auto occupied = vm::Mapping::reserve(2 * page, nxrt::mem::kMainBase);
    REQUIRE(occupied);
    REQUIRE(occupied.protect(0, 2 * page, vm::Access::ReadWrite));
    byte(occupied) = 88;
    CHECK(!object.map(0, page, vm::Access::ReadWrite, nxrt::mem::kMainBase));
    CHECK(!object.map(page, 2 * page, vm::Access::ReadWrite));
    CHECK(!object.map(1, page, vm::Access::ReadWrite));
    CHECK(!object.map(0, page - 1, vm::Access::ReadWrite));
    CHECK_EQ(byte(occupied), 88);
}

NXTEST(views_survive_object_close_and_move_ownership) {
    const size_t page = vm::page_size();
    auto mapping = [page] {
        auto original = vm::SharedObject::create(page);
        auto moved = std::move(original);
        CHECK(!original);
        CHECK_EQ(original.size(), size_t{0});
        CHECK(moved);
        return moved.map(0, page, vm::Access::ReadWrite);
    }();
    REQUIRE(mapping);
    byte(mapping) = 34;
    auto moved = std::move(mapping);
    CHECK(!mapping);
    CHECK_EQ(mapping.size(), size_t{0});
    CHECK_EQ(byte(moved), 34);
    CHECK(mapping.reset());
    CHECK_EQ(byte(moved), 34);
}

NXTEST(full_guest_reservations_touch_only_three_host_pages) {
    // Check the real address-space sizes without committing a game-sized heap.
    // Sparse backing reserves 12 GiB; this test touches just two heap pages and
    // one stack page. It does not establish the available physical RAM budget.
    const size_t page = vm::page_size();
#ifdef NXRT_TEST_PS5
    namespace native = nxrt::ps5_vm;
    auto stack = native::Reservation::reserve(nxrt::mem::kStackSize, nxrt::mem::kStackBase);
    auto kernel = native::Reservation::reserve(nxrt::mem::kKernelSize, nxrt::mem::kKernelBase);
    auto heap = native::Reservation::reserve(nxrt::mem::kHeapMax, nxrt::mem::kHeapBase);
    auto alias = native::Reservation::reserve(page, nxrt::mem::kAliasBase);
    REQUIRE(stack && kernel && heap && alias);
    {
        auto backing = native::DirectMemory::allocate(3 * page);
        REQUIRE(backing);
        REQUIRE(heap.map(0, backing, 0, page, vm::Access::ReadWrite));
        REQUIRE(heap.map(heap.size() - page, backing, page, page, vm::Access::ReadWrite));
        REQUIRE(stack.map(0, backing, 2 * page, page, vm::Access::ReadWrite));
        REQUIRE(alias.map(0, backing, page, page, vm::Access::Read));
    } // All views must retain the backing independently of its original handle.
    auto* first = static_cast<unsigned char*>(heap.data());
    auto* last = first + heap.size() - page;
    *first = 9; *last = 11;
    *static_cast<unsigned char*>(stack.data()) = 7;
    CHECK_EQ(*static_cast<unsigned char*>(alias.data()), 11);
    CHECK_EQ(*first, 9);
    CHECK_EQ(*static_cast<unsigned char*>(stack.data()), 7);
#else
    auto stack = vm::Mapping::reserve(nxrt::mem::kStackSize, nxrt::mem::kStackBase);
    if (!stack) std::printf("stack reservation errno=%d\n", errno);
    REQUIRE(stack);
    auto kernel = vm::Mapping::reserve(nxrt::mem::kKernelSize, nxrt::mem::kKernelBase);
    if (!kernel) std::printf("kernel reservation errno=%d\n", errno);
    REQUIRE(kernel);
    auto backing = vm::SharedObject::create(nxrt::mem::kHeapMax);
    if (!backing) std::printf("heap backing errno=%d\n", errno);
    REQUIRE(backing);
    auto heap = backing.map(0, nxrt::mem::kHeapMax, vm::Access::None, nxrt::mem::kHeapBase);
    REQUIRE(heap);
    REQUIRE(heap.protect(0, page, vm::Access::ReadWrite));
    REQUIRE(heap.protect(heap.size() - page, page, vm::Access::ReadWrite));
    REQUIRE(stack.protect(0, page, vm::Access::ReadWrite));
    byte(stack) = 7;
    byte(heap) = 9;
    byte(heap, heap.size() - page) = 11;
    auto alias = backing.map(heap.size() - page, page, vm::Access::Read, nxrt::mem::kAliasBase);
    REQUIRE(alias);
    CHECK_EQ(byte(alias), 11);
    CHECK_EQ(byte(heap), 9);
    CHECK_EQ(byte(stack), 7);
#endif
}

#ifdef NXRT_TEST_PS5
NXTEST(native_views_reject_overlap_and_keep_backing_until_last_view) {
    namespace native = nxrt::ps5_vm;
    const size_t page = vm::page_size();
    FaultScope faults;
    REQUIRE(faults.segv_set && faults.bus_set);
    auto mapping = native::Reservation::reserve(4 * page, nxrt::mem::kMainBase);
    auto alias = native::Reservation::reserve(page, nxrt::mem::kAliasBase);
    REQUIRE(mapping && alias);
    CHECK(!native::Reservation::reserve(4 * page, nxrt::mem::kMainBase));
    CHECK(!mapping.protect(0, page, vm::Access::ReadWrite));
    {
        auto backing = native::DirectMemory::allocate(2 * page);
        REQUIRE(backing);
        CHECK(!mapping.map(0, backing, 0, page / 2, vm::Access::ReadWrite));
        CHECK(!mapping.map(0, backing, page, 2 * page, vm::Access::ReadWrite));
        REQUIRE(mapping.map(0, backing, 0, 2 * page, vm::Access::ReadWrite));
        REQUIRE(alias.map(0, backing, 0, page, vm::Access::ReadWrite));
        CHECK(!mapping.map(page, backing, 0, page, vm::Access::ReadWrite));
    }
    CHECK(can_touch(mapping.data(), 0, true));
    REQUIRE(mapping.protect(0, page, vm::Access::Read));
    CHECK(!can_touch(mapping.data(), 0, true));
    CHECK(can_touch(mapping.data(), page, true));
    CHECK(!mapping.protect(0, 3 * page, vm::Access::None));
    CHECK(!mapping.protect(0, page / 2, vm::Access::None));
    CHECK(can_touch(alias.data(), 0, true));
    REQUIRE(mapping.reset());
    CHECK(can_touch(alias.data(), 0, true));
    REQUIRE(alias.reset());
    auto reused = native::Reservation::reserve(4 * page, nxrt::mem::kMainBase);
    REQUIRE(reused);
}
#endif
