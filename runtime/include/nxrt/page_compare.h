// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace nxrt::mem {
// Both snapshots contain a complete 4 KiB page. Unaligned loads accommodate
// map nodes as well as stack copies; every byte participates in the result.
inline bool page_equal(const uint8_t* a, const uint8_t* b) {
#if defined(__SSE2__)
    const auto zero = _mm_setzero_si128();
    for (size_t offset = 0; offset < 4096; offset += 64) {
        auto difference = zero;
        for (size_t lane = 0; lane < 64; lane += 16) {
            const auto left = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + offset + lane));
            const auto right = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + offset + lane));
            difference = _mm_or_si128(difference, _mm_xor_si128(left, right));
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(difference, zero)) != 0xffff) return false;
    }
    return true;
#else
    return std::memcmp(a, b, 4096) == 0;
#endif
}
}
