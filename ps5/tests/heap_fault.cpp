// SPDX-License-Identifier: GPL-3.0-or-later
#include "heap_fault.h"
#include <atomic>
#include <cstdint>
namespace {
std::atomic<unsigned> countdown{0};
std::atomic<size_t> allocated{0}, released{0};
}
namespace heap_fault {
void fail_allocation(unsigned call) { countdown = call; }
size_t allocations() { return allocated.load(); }
size_t releases() { return released.load(); }
}
extern "C" int __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
extern "C" int __real_sceKernelReleaseDirectMemory(int64_t, size_t);
extern "C" int __wrap_sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t size,
                                                    size_t alignment, int type, int64_t* physical) {
    unsigned left = countdown.load();
    while (left && !countdown.compare_exchange_weak(left, left - 1)) {}
    if (left == 1) return static_cast<int>(0x8002000cu);
    const int rc = __real_sceKernelAllocateDirectMemory(start, end, size, alignment, type, physical);
    if (rc == 0) ++allocated;
    return rc;
}
extern "C" int __wrap_sceKernelReleaseDirectMemory(int64_t physical, size_t size) {
    const int rc = __real_sceKernelReleaseDirectMemory(physical, size);
    if (rc == 0) ++released;
    return rc;
}
