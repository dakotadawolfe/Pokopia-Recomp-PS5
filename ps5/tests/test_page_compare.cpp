// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/page_compare.h"
#include <array>
#include <chrono>
#include <cstdio>

NXTEST_GROUP(page_compare)

NXTEST(page_comparison_detects_every_byte_and_accepts_unaligned_snapshots) {
    std::array<uint8_t, 4096 + 32> left{}, right{};
    for (unsigned alignment : {0u, 1u, 7u, 15u}) {
        auto* a = left.data() + alignment;
        auto* b = right.data() + 31 - alignment;
        for (size_t i = 0; i < 4096; ++i) a[i] = b[i] = static_cast<uint8_t>(i * 73 + 19);
        REQUIRE(nxrt::mem::page_equal(a, b));
        for (size_t i = 0; i < 4096; ++i) {
            b[i] ^= static_cast<uint8_t>(1u << (i % 8));
            CHECK(!nxrt::mem::page_equal(a, b));
            b[i] = a[i];
        }
        CHECK(nxrt::mem::page_equal(a, b));
    }
}

NXTEST(page_comparison_benchmark_reports_the_native_library_baseline) {
    std::array<uint8_t, 4096> left{}, right{};
    for (size_t i = 0; i < left.size(); ++i) left[i] = right[i] = static_cast<uint8_t>(i * 31);
    int (*volatile library_compare)(const void*, const void*, size_t) = std::memcmp;
    bool (*volatile vector_compare)(const uint8_t*, const uint8_t*) = nxrt::mem::page_equal;
    unsigned library_matches = 0, vector_matches = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 32768; ++i) library_matches += library_compare(left.data(), right.data(), left.size()) == 0;
    const auto middle = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 32768; ++i) vector_matches += vector_compare(left.data(), right.data());
    const auto end = std::chrono::steady_clock::now();
    CHECK_EQ(library_matches, 32768u);
    CHECK_EQ(vector_matches, 32768u);
    std::printf("PAGE_COMPARE_BENCH pages=32768 library_us=%lld vector_us=%lld\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(middle - start).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(end - middle).count()));
}
