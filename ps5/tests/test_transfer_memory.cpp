// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/subpage.h"
#include "nxrt/svc.h"
#include <cstring>

namespace nxrt { void svc_memory_reset_for_tests(); }
NXTEST_GROUP(transfer_memory)
namespace {
namespace k = nxrt::kernel;
namespace m = nxrt::mem;
uint32_t svc(Ctx& c, unsigned number) { nxrt::svc_dispatch(&c, number); return c.x[0]; }
uint32_t borrow(Ctx& c, uint64_t address, uint64_t size, uint32_t permission) {
    c = {}; c.x[1] = address; c.x[2] = size; c.x[3] = permission; return svc(c, 0x15);
}
struct Fixture {
    uint64_t info = 0;
    Fixture() {
        k::kernel_reset(); nxrt::svc_memory_reset_for_tests(); m::shutdown();
        if (!m::init({.heap_reserve = 8ull << 20})) std::abort();
        nxrt::svc_init(); Ctx c{}; c.x[1] = 4 << 20;
        if (svc(c, 1) || !(info = m::alloc_kernel(0x1000))) std::abort();
    }
    ~Fixture() { k::kernel_reset(); nxrt::svc_memory_reset_for_tests(); m::shutdown(); }
    uint32_t attribute(uint64_t address, uint64_t expected_size = 0x1000) {
        Ctx c{}; c.x[0] = info; c.x[2] = address; CHECK_EQ(svc(c, 6), 0u);
        uint64_t range[2]; uint32_t fields[3];
        std::memcpy(range, reinterpret_cast<void*>(info), sizeof range);
        std::memcpy(fields, reinterpret_cast<void*>(info + 16), sizeof fields);
        CHECK_EQ(range[0], address); CHECK_EQ(range[1], expected_size); CHECK_EQ(fields[0], m::kMemHeap);
        return fields[1];
    }
};
}

NXTEST(transfer_owner_permissions_private_alias_and_last_reference) {
    Fixture f;
    const uint64_t address = m::kHeapBase + 0x1000;
    for (unsigned permission : {0u, 1u, 3u}) {
        *reinterpret_cast<uint32_t*>(address) = 0x11223344;
        *reinterpret_cast<uint32_t*>(address - 4) = 0xaabbccdd;
        Ctx c{}; REQUIRE(borrow(c, address, 0x1000, permission) == 0);
        const uint32_t h = c.x[1]; auto tm = k::handle_as<k::KTransferMemory>(h);
        REQUIRE(tm && tm->host && tm->host != address);
        CHECK_EQ(*reinterpret_cast<uint32_t*>(tm->host), 0x11223344u);
        CHECK_EQ(m::SubpageMap::allows(address, 4, 1), permission != 0);
        CHECK_EQ(m::SubpageMap::allows(address, 4, 2), permission == 3);
        CHECK_EQ(f.attribute(address), 1u);
        *reinterpret_cast<uint32_t*>(tm->host) = 0x55667788;
        if (permission) CHECK_EQ(*reinterpret_cast<uint32_t*>(address), 0x55667788u);
        CHECK_EQ(*reinterpret_cast<uint32_t*>(address - 4), 0xaabbccddu);
        k::handle_close(h); CHECK_EQ(f.attribute(address), 1u);
        tm.reset();
        CHECK(m::SubpageMap::allows(address, 0x1000, 3));
        CHECK_EQ(*reinterpret_cast<uint32_t*>(address), 0x55667788u);
    }
}

NXTEST(transfer_owner_cannot_be_reborrowed_reprotected_mapped_or_shrunk) {
    Fixture f; const uint64_t address = m::kHeapBase + 0x201000;
    Ctx c{}; REQUIRE(borrow(c, address, 0x1000, 3) == 0); const uint32_t h = c.x[1];
    CHECK_EQ(borrow(c, address, 0x2000, 3), k::kInvalidMemoryState);
    c = {}; c.x[0] = address; c.x[1] = 0x1000; c.x[2] = 1; CHECK_EQ(svc(c, 2), k::kInvalidMemoryState);
    c = {}; c.x[0] = m::kStackBase; c.x[1] = address; c.x[2] = 0x1000; CHECK_EQ(svc(c, 4), k::kInvalidMemoryState);
    c = {}; c.x[1] = 2 << 20; CHECK_EQ(svc(c, 1), k::kInvalidMemoryState);
    CHECK_EQ(f.attribute(address), 1u);
    k::handle_close(h);
    c = {}; c.x[1] = 2 << 20; CHECK_EQ(svc(c, 1), 0u);
}

NXTEST(transfer_validates_ranges_permissions_and_rolls_back_full_handle_table) {
    Fixture f; const uint64_t address = m::kHeapBase + 0x1000; Ctx c{};
    CHECK_EQ(borrow(c, address + 1, 0x1000, 0), k::kInvalidAddress);
    CHECK_EQ(borrow(c, address, 0, 0), k::kInvalidSize);
    CHECK_EQ(borrow(c, address, 0x1001, 0), k::kInvalidSize);
    CHECK_EQ(borrow(c, UINT64_MAX - 0xfff, 0x2000, 0), k::kInvalidMemoryState);
    CHECK_EQ(borrow(c, address, 0x1000, 2), k::kInvalidPermission);
    CHECK_EQ(borrow(c, m::kHeapBase + (4 << 20), 0x1000, 0), k::kInvalidMemoryState);
    const uint64_t alias = m::heap_alias(0, 0x10000); REQUIRE(alias);
    CHECK_EQ(borrow(c, address, 0x1000, 0), k::kInvalidMemoryState);
    REQUIRE(m::heap_alias_unmap(alias));
    auto object = std::make_shared<k::KObject>();
    for (unsigned i = 0; i < 0x3000; ++i) REQUIRE(k::handle_add(object));
    CHECK_EQ(borrow(c, address, 0x1000, 0), k::kOutOfHandles);
    CHECK(m::SubpageMap::allows(address, 0x1000, 3));
    k::handles_reset(); REQUIRE(borrow(c, address, 0x1000, 0) == 0); k::handle_close(c.x[1]);
}
