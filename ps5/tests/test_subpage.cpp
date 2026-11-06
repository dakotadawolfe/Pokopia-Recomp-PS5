// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/host_vm.h"
#include "nxrt/subpage.h"
#include "nxrt/memory.h"
#include "nxrt/abi.h"
#include "nxrecomp/cxx.h"
#include <atomic>
#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <thread>

NXTEST_GROUP(subpage)
extern "C" const NxModule nxmod_subpage_fixture;
extern "C" const uint32_t nxmod_subpage_fixture_abi;
namespace {
constexpr uint64_t base = nxrt::mem::kMainBase;
constexpr uint64_t page = 4096, host = 16384;
using nxrt::mem::SubpageMap;
using nxrt::host_vm::Access;
sigjmp_buf fault_return;
volatile sig_atomic_t expected_fault = 0;
void on_fault(int signal) {
    if (expected_fault) siglongjmp(fault_return, 1);
    _Exit(128 + signal);
}
struct FaultScope {
    struct sigaction previous{};
    bool installed = false;
    FaultScope() {
        struct sigaction action{};
        action.sa_handler = on_fault;
        sigemptyset(&action.sa_mask);
        installed = sigaction(SIGSEGV, &action, &previous) == 0;
    }
    ~FaultScope() { if (installed) sigaction(SIGSEGV, &previous, nullptr); }
};
bool executes(void (*operation)(uint64_t), uint64_t address) {
    if (sigsetjmp(fault_return, 1)) { expected_fault = 0; return false; }
    expected_fault = 1;
    operation(address);
    expected_fault = 0;
    return true;
}
struct Region {
    nxrt::host_vm::Mapping mapping = nxrt::host_vm::Mapping::reserve(4 * host, base);
    unsigned apply_calls = 0, fail_on = 0;
    std::unique_ptr<SubpageMap> policy;
    Region() {
        if (!mapping || !mapping.protect(0, mapping.size(), Access::ReadWrite)) return;
        std::memset(mapping.data(), 0, mapping.size());
        if (!mapping.protect(0, mapping.size(), Access::None)) return;
        policy = SubpageMap::create(base, mapping.size(), [this](uint64_t a, uint64_t size, Access access) {
            if (++apply_calls == fail_on) return false;
            return mapping.protect(a - base, size, access);
        });
    }
};
void read_word(uint64_t a) { volatile auto value = rd32(a); (void)value; }
void write_word(uint64_t a) { wr32(a, 0x98765432); }
void write_wide(uint64_t a) { wr64(a, UINT64_C(0x0123456789abcdef)); }
void write_vector(uint64_t a) { V128 v{}; v.d[0] = 1; v.d[1] = 2; wrv(a, &v, 16); }
void cpp_atomic(uint64_t a) { Ctx c{}; c.ex_addr = a; c.ex_val = nx::ld_acq<uint64_t>(a); nx::stxr(&c, a, uint64_t{4}); }
void c_atomic(uint64_t a) { Ctx c{}; c.ex_addr = a; c.ex_val = rd64(a); stx64(&c, a, 5); }
void large_write_check(uint64_t a) { nx_check_memory(a, 3 * host, 2); }
Ctx lifted(unsigned index, uint64_t address) {
    const uint64_t pc = base + index * 64;
    for (int i = 0; i < nxmod_subpage_fixture.nfns; ++i) {
        if (nxmod_subpage_fixture.fns[i].addr == pc) {
            Ctx c{}; c.x[0] = 1; c.x[1] = address; c.x[2] = 2;
            c.v[0].d[0] = 3; c.v[0].d[1] = 4;
            nxmod_subpage_fixture.fns[i].fn(&c);
            return c;
        }
    }
    nxtest::fail(__FILE__, __LINE__, "fixture function present", std::to_string(index));
    return {};
}
template<unsigned Index> void fixture(uint64_t a) { lifted(Index, a); }
}

NXTEST(subpage_registration_and_range_validation) {
    CHECK_EQ(nxmod_subpage_fixture_abi, nxrt::kAbiVersion);
    Region region;
    REQUIRE(region.policy);
    CHECK(!SubpageMap::create(base, host, [](auto, auto, auto) { return true; }));
    CHECK(!SubpageMap::create(base + page, host, [](auto, auto, auto) { return true; }));
    CHECK(!region.policy->protect(base + 1, page, 3));
    CHECK(!region.policy->protect(base, page - 1, 3));
    CHECK(!region.policy->protect(base, page, 2));
    CHECK(!region.policy->protect(base - page, page, 3));
    CHECK(!region.policy->protect(base, 5 * host, 3));
    CHECK(!SubpageMap::allows(UINT64_MAX - 3, 8, 1));
}

NXTEST(readonly_guest_page_keeps_writable_neighbor) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    wr32(base, 17);
    REQUIRE(region.policy->protect(base, page, 1));
    CHECK_EQ(rd32(base), uint32_t{17});
    CHECK(!executes(write_word, base));
    CHECK(executes(write_word, base + page));
    CHECK_EQ(rd32(base), uint32_t{17});
    CHECK_EQ(nx_guest_subpage_flags[base >> 14], uint8_t{1});
    REQUIRE(region.policy->protect(base, page, 3));
    CHECK_EQ(nx_guest_subpage_flags[base >> 14], uint8_t{0});
    CHECK(executes(write_word, base));
}

NXTEST(noaccess_guest_page_and_cross_page_stores_fault_before_writing) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    wr32(base + page - 4, 0x11223344);
    REQUIRE(region.policy->protect(base + page, page, 0));
    CHECK(!executes(read_word, base + page));
    CHECK(!executes(write_word, base + page));
    CHECK(!executes(write_wide, base + page - 4));
    CHECK(!executes(write_vector, base + page - 8));
    CHECK_EQ(rd32(base + page - 4), uint32_t{0x11223344});
    CHECK(executes(write_word, base + 2 * page));
}

NXTEST(c_and_cpp_atomic_helpers_enforce_write_permissions) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    REQUIRE(region.policy->protect(base, page, 1));
    CHECK(!executes(c_atomic, base));
    CHECK(!executes(cpp_atomic, base));
    CHECK(executes(c_atomic, base + page));
    CHECK(executes(cpp_atomic, base + page));
}

NXTEST(lifted_atomic_vector_and_cache_instructions_check_guest_permissions) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    REQUIRE(region.policy->protect(base, page, 1));
    CHECK(executes(fixture<0>, base)); // LDAR reads the read-only page.
    CHECK(executes(fixture<7>, base)); // LDAXP W pair uses the direct tier-1 atomic path.
    for (auto instruction : {fixture<1>, fixture<2>, fixture<3>, fixture<4>, fixture<5>, fixture<6>}) {
        CHECK(!executes(instruction, base));
        CHECK(executes(instruction, base + page));
    }
    REQUIRE(region.policy->protect(base, page, 0));
    CHECK(!executes(fixture<0>, base));
    CHECK(!executes(fixture<7>, base));
}

NXTEST(lifted_instructions_preserve_register_and_memory_results) {
    Region region;
    REQUIRE(region.policy);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    REQUIRE(region.policy->protect(base, page, 1));
    const uint64_t a = base + page;
    wr64(a, 37);
    CHECK_EQ(lifted(0, a).x[0], uint64_t{37});
    lifted(1, a); CHECK_EQ(rd64(a), uint64_t{1});
    wr64(a, 37);
    auto exclusive = lifted(2, a);
    CHECK_EQ(exclusive.x[2], uint64_t{37});
    CHECK_EQ(exclusive.x[3], uint64_t{0});
    CHECK_EQ(rd64(a), uint64_t{1});
    auto cas = lifted(3, a);
    CHECK_EQ(cas.x[0], uint64_t{1});
    CHECK_EQ(rd64(a), uint64_t{2});
    wr64(a, 37);
    auto swap = lifted(4, a);
    CHECK_EQ(swap.x[2], uint64_t{37});
    CHECK_EQ(rd64(a), uint64_t{1});
    lifted(5, a);
    CHECK_EQ(rd64(a), uint64_t{3}); CHECK_EQ(rd64(a + 8), uint64_t{4});
    for (unsigned i = 0; i < 64; ++i) wr8(a + i, 0x7b);
    lifted(6, a);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(rd8(a + i), uint8_t{0});
    wr64(a, UINT64_C(0x1122334455667788));
    auto pair = lifted(7, a);
    CHECK_EQ(pair.x[2], uint64_t{0x55667788});
    CHECK_EQ(pair.x[3], uint64_t{0x11223344});
}

NXTEST(long_access_checks_mixed_interior_blocks) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    REQUIRE(region.policy->protect(base + host, page, 1));
    CHECK_EQ(nx_guest_subpage_flags[base >> 14], uint8_t{0});
    CHECK_EQ(nx_guest_subpage_flags[(base + 2 * host) >> 14], uint8_t{0});
    CHECK(!executes(large_write_check, base));
}

NXTEST(protection_failure_rolls_back_host_and_guest_state) {
    Region region;
    FaultScope faults;
    REQUIRE(region.policy && faults.installed);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    REQUIRE(region.policy->protect(base, host, 1));
    region.apply_calls = 0;
    region.fail_on = 2;
    CHECK(!region.policy->protect(base, 2 * host, 0));
    CHECK(SubpageMap::allows(base, host, 1));
    CHECK(!SubpageMap::allows(base, host, 2));
    CHECK(SubpageMap::allows(base + host, host, 3));
    CHECK(executes(read_word, base));
    CHECK(!executes(write_word, base));
    CHECK(executes(write_word, base + host));
}

NXTEST(uniform_runs_coalesce_and_concurrent_readers_keep_access) {
    Region region;
    REQUIRE(region.policy);
    REQUIRE(region.policy->protect(base, 4 * host, 3));
    CHECK_EQ(region.apply_calls, 1u);
    wr32(base + page, 0x1234);
    std::atomic<bool> stop{false};
    std::atomic<bool> correct{true};
    std::thread reader([&] {
        while (!stop.load()) if (rd32(base + page) != 0x1234) correct.store(false);
    });
    for (int i = 0; i < 100; ++i) {
        CHECK(region.policy->protect(base, page, 0));
        CHECK(region.policy->protect(base, page, 3));
    }
    stop.store(true); reader.join();
    CHECK(correct.load());
    CHECK_EQ(region.apply_calls, 1u); // Logical changes needed no host reprotection.
}

NXTEST(cached_permissions_revalidate_changes_and_cross_page_accesses) {
    Region region;
    REQUIRE(region.policy);
    REQUIRE(region.policy->protect(base, host, 3));
    CHECK(SubpageMap::allows(base, 4, 3));
    REQUIRE(region.policy->protect(base + page, page, 1));
    CHECK(SubpageMap::allows(base, 4, 3));
    CHECK(!SubpageMap::allows(base + page - 4, 8, 2));
    CHECK(SubpageMap::allows(base + page - 4, 8, 1));
    REQUIRE(region.policy->protect(base, page, 0));
    CHECK(!SubpageMap::allows(base, 4, 1));
    REQUIRE(region.policy->protect(base, page, 1));
    CHECK(SubpageMap::allows(base, 4, 1));
    CHECK(!SubpageMap::allows(base, 4, 2));
    REQUIRE(region.policy->protect(base, page, 3));
    CHECK(SubpageMap::allows(base, 4, 3));
}

NXTEST(cached_permissions_do_not_survive_mapping_replacement) {
    {
        Region region;
        REQUIRE(region.policy);
        REQUIRE(region.policy->protect(base, host, 3));
        CHECK(SubpageMap::allows(base, 4, 3));
    }
    CHECK(!SubpageMap::allows(base, 4, 1));
    Region replacement;
    REQUIRE(replacement.policy);
    REQUIRE(replacement.policy->protect(base, page, 1));
    CHECK(SubpageMap::allows(base, 4, 1));
    CHECK(!SubpageMap::allows(base, 4, 2));
}

NXTEST(cached_permissions_observe_another_threads_revocation_and_restore) {
    Region region;
    REQUIRE(region.policy);
    REQUIRE(region.policy->protect(base, host, 3));
    std::atomic<unsigned> phase{0}, acknowledged{0};
    std::atomic<bool> correct{true};
    std::thread reader([&] {
        if (!SubpageMap::allows(base, 4, 3)) correct = false;
        acknowledged = 1;
        while (phase.load() != 1) std::this_thread::yield();
        if (SubpageMap::allows(base, 4, 2) || !SubpageMap::allows(base, 4, 1)) correct = false;
        acknowledged = 2;
        while (phase.load() != 2) std::this_thread::yield();
        if (!SubpageMap::allows(base, 4, 3)) correct = false;
    });
    while (acknowledged.load() != 1) std::this_thread::yield();
    CHECK(region.policy->protect(base, page, 1));
    phase = 1;
    while (acknowledged.load() != 2) std::this_thread::yield();
    CHECK(region.policy->protect(base, page, 3));
    phase = 2;
    reader.join();
    CHECK(correct.load());
}
