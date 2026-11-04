// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "../app/heap.h"
#include "heap_fault.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

extern "C" void* __real_malloc(size_t);
extern "C" size_t malloc_usable_size(const void*);
NXTEST_GROUP(app_heap)
namespace heap = nxrt::app_heap;
namespace {
constexpr size_t mib = 1u << 20;
bool pattern(const void* data, size_t size, unsigned char value) {
    const auto* p = static_cast<const unsigned char*>(data);
    return std::all_of(p, p + size, [value](unsigned char byte) { return byte == value; });
}
}

NXTEST(native_heap_holds_simultaneous_large_cpp_buffers) {
    const auto before = heap::stats();
    {
        std::array<std::vector<unsigned char>, 4> blocks;
        for (size_t i = 0; i < blocks.size(); ++i) {
            blocks[i].resize(24 * mib, static_cast<unsigned char>(i + 1));
            CHECK(heap::owns(blocks[i].data()));
        }
        CHECK(heap::stats().backing_bytes >= 96 * mib);
        for (size_t i = 0; i < blocks.size(); ++i)
            CHECK(pattern(blocks[i].data(), blocks[i].size(), static_cast<unsigned char>(i + 1)));
    }
    CHECK_EQ(heap::stats().live_blocks, before.live_blocks);
    CHECK_EQ(heap::stats().live_bytes, before.live_bytes);
}
NXTEST(native_heap_realloc_preserves_contents_and_failure_ownership) {
    const auto before = heap::stats();
    auto* p = static_cast<unsigned char*>(std::calloc(3, mib));
    REQUIRE(p);
    CHECK(heap::owns(p));
    CHECK(pattern(p, 3 * mib, 0));
    std::memset(p, 0x6d, 3 * mib);
    auto* grown = static_cast<unsigned char*>(std::realloc(p, 9 * mib));
    if (!grown) { std::free(p); REQUIRE(grown); }
    CHECK(pattern(grown, 3 * mib, 0x6d));
    volatile size_t impossible = std::numeric_limits<size_t>::max();
    errno = 0;
    void* failed = std::realloc(grown, impossible);
    REQUIRE(!failed);
    CHECK_EQ(errno, ENOMEM);
    CHECK(pattern(grown, 3 * mib, 0x6d));
    CHECK(!std::realloc(grown, 0));
    CHECK(!std::calloc(impossible, 2));
    void* zero = std::malloc(0);
    CHECK(zero != nullptr);
    std::free(zero);
    std::free(nullptr);
    CHECK_EQ(heap::stats().live_blocks, before.live_blocks);
    CHECK_EQ(heap::stats().live_bytes, before.live_bytes);
}
NXTEST(native_heap_alignment_and_posix_failure_contract) {
    void* p = reinterpret_cast<void*>(uintptr_t{0x1234});
    errno = EDOM;
    CHECK_EQ(posix_memalign(&p, 3, mib), EINVAL);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p), uintptr_t{0x1234});
    CHECK_EQ(errno, EDOM);
    volatile size_t impossible = std::numeric_limits<size_t>::max();
    CHECK_EQ(posix_memalign(&p, 16384, impossible), ENOMEM);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p), uintptr_t{0x1234});
    CHECK_EQ(errno, EDOM);
    for (size_t alignment : {size_t{16}, size_t{16384}, 2 * mib}) {
        REQUIRE(posix_memalign(&p, alignment, 3 * mib) == 0);
        CHECK_EQ(reinterpret_cast<uintptr_t>(p) % alignment, size_t{0});
        CHECK(heap::owns(p));
        CHECK(malloc_usable_size(p) >= 3 * mib);
        std::memset(p, 0xc5, 3 * mib);
        CHECK(pattern(p, 3 * mib, 0xc5));
        std::free(p);
    }
    p = std::aligned_alloc(16384, mib);
    REQUIRE(p);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 16384, size_t{0});
    std::free(p);
    CHECK(!std::aligned_alloc(16384, mib + 1));
    CHECK_EQ(errno, EINVAL);
}
NXTEST(native_heap_returns_foreign_allocations_to_their_owner) {
    const auto before = heap::stats();
    void* p = __real_malloc(64);
    REQUIRE(p);
    CHECK(!heap::owns(p));
    std::memset(p, 0x47, 64);
    void* grown = std::realloc(p, 96);
    if (!grown) { std::free(p); REQUIRE(grown); }
    CHECK(!heap::owns(grown));
    CHECK(pattern(grown, 64, 0x47));
    CHECK(malloc_usable_size(grown) >= 96);
    std::free(grown);
    CHECK_EQ(heap::stats().live_blocks, before.live_blocks);
}
NXTEST(native_heap_allocates_concurrently_and_frees_across_threads) {
    std::array<std::array<void*, 8>, 4> returned{};
    std::atomic<unsigned> errors{0};
    std::array<std::thread, 4> workers;
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            for (size_t j = 0; j < 128; ++j) {
                const size_t size = 37 + (j % 16) * 4096;
                void* p = std::malloc(size);
                if (!p) { ++errors; continue; }
                std::memset(p, static_cast<int>(i + 1), size);
                if (!heap::owns(p) || !pattern(p, size, static_cast<unsigned char>(i + 1))) ++errors;
                std::free(p);
            }
            for (auto& p : returned[i]) {
                p = std::malloc(mib);
                if (p) std::memset(p, static_cast<int>(i + 1), mib);
                else ++errors;
            }
        });
    }
    for (auto& thread : workers) thread.join();
    for (size_t i = 0; i < returned.size(); ++i) for (void* p : returned[i]) {
        if (p) CHECK(pattern(p, mib, static_cast<unsigned char>(i + 1)));
        std::free(p);
    }
    CHECK_EQ(errors.load(), 0u);
}
NXTEST(native_heap_reuses_large_scratch_space) {
    const auto before = heap::stats();
    size_t backing = 0;
    for (unsigned i = 0; i < 12; ++i) {
        void* p = std::malloc(40 * mib);
        REQUIRE(p);
        std::memset(p, static_cast<int>(i), 40 * mib);
        CHECK(pattern(p, 40 * mib, static_cast<unsigned char>(i)));
        std::free(p);
        if (!i) backing = heap::stats().backing_bytes;
        else CHECK_EQ(heap::stats().backing_bytes, backing);
    }
    CHECK_EQ(heap::stats().live_blocks, before.live_blocks);
    CHECK_EQ(heap::stats().live_bytes, before.live_bytes);
}
NXTEST(native_heap_rolls_back_partial_physical_allocation) {
    const auto before = heap::stats();
    const auto allocated = heap_fault::allocations(), released = heap_fault::releases();
    heap_fault::fail_allocation(2);
    void* p = std::malloc(96 * mib);
    heap_fault::fail_allocation(0);
    CHECK(!p);
    if (p) { std::free(p); return; }
    CHECK_EQ(heap_fault::allocations() - allocated, size_t{1});
    CHECK_EQ(heap_fault::releases() - released, size_t{1});
    CHECK_EQ(heap::stats().spaces, before.spaces);
    CHECK_EQ(heap::stats().backing_bytes, before.backing_bytes);
    p = std::malloc(96 * mib);
    REQUIRE(p);
    std::memset(p, 0x76, 96 * mib);
    CHECK(pattern(p, 96 * mib, 0x76));
    std::free(p);
    CHECK_EQ(heap::stats().live_blocks, before.live_blocks);
}
