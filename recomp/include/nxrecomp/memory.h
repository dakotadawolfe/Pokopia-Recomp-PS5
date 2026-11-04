/* SPDX-License-Identifier: GPL-3.0-or-later
 * Extra permission checks for hosts whose pages contain multiple guest pages.
 * Uniform pages still rely on the host MMU. A byte per 16 KiB host page marks
 * mixed permissions or an in-progress mapping change. Valid C11 and C++17. */
#ifndef NXRECOMP_MEMORY_H
#define NXRECOMP_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef NXRECOMP_SUBPAGE_CHECKS
#define NX_GUEST_ADDRESS_END UINT64_C(0x8000000000)
#define NX_GUEST_HOST_PAGE_SHIFT 14
extern uint8_t nx_guest_subpage_flags[NX_GUEST_ADDRESS_END >> NX_GUEST_HOST_PAGE_SHIFT];
void nx_guest_subpage_slow(uint64_t address, size_t size, unsigned access);
static inline void nx_check_memory(uint64_t address, size_t size, unsigned access) {
    if (!size) return;
    if (size - 1 > UINT64_MAX - address) {
        nx_guest_subpage_slow(address, size, access);
        return;
    }
    const uint64_t end = address + size - 1;
    if (address >= NX_GUEST_ADDRESS_END) return; /* Runtime-owned host pointers. */
    if (end >= NX_GUEST_ADDRESS_END) {
        nx_guest_subpage_slow(address, size, access);
        return;
    }
    const uint64_t first = address >> NX_GUEST_HOST_PAGE_SHIFT;
    const uint64_t last = end >> NX_GUEST_HOST_PAGE_SHIFT;
    if (__atomic_load_n(nx_guest_subpage_flags + first, __ATOMIC_ACQUIRE) ||
        (first != last && __atomic_load_n(nx_guest_subpage_flags + last, __ATOMIC_ACQUIRE))) {
        nx_guest_subpage_slow(address, size, access);
        return;
    }
    /* Scalar, vector, atomic and cache-zero helpers never need this loop. */
    for (uint64_t block = first + 1; size > (1u << NX_GUEST_HOST_PAGE_SHIFT) && block < last; ++block) {
        if (__atomic_load_n(nx_guest_subpage_flags + block, __ATOMIC_ACQUIRE)) {
            nx_guest_subpage_slow(address, size, access);
            return;
        }
    }
}
#else
static inline void nx_check_memory(uint64_t address, size_t size, unsigned access) {
    (void)address; (void)size; (void)access;
}
#endif
static inline uint64_t nx_checked_address(uint64_t address, size_t size, unsigned access) {
    nx_check_memory(address, size, access);
    return address;
}
#ifdef __cplusplus
}
#endif
#endif
