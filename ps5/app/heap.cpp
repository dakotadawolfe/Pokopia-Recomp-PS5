// SPDX-License-Identifier: GPL-3.0-or-later
// Native applications cannot use the small libc internal heap for game data.
// Give the platform's mspace allocator direct memory as demand grows. This
// layer must not allocate its own bookkeeping through malloc, TLS or C++ new.
#include "heap.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sys/mman.h>

extern "C" {
void* __real_malloc(size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
size_t __real_malloc_usable_size(const void*);
void* sceLibcMspaceCreate(const char*, void*, size_t, unsigned);
void* sceLibcMspaceMalloc(void*, size_t);
void sceLibcMspaceFree(void*, void*);
int sceLibcMspacePosixMemalign(void*, void**, size_t, size_t);
size_t sceLibcMspaceMallocUsableSize(const void*);
int64_t sceKernelGetDirectMemorySize();
int sceKernelReserveVirtualRange(void**, size_t, int, size_t);
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
int sceKernelUsleep(unsigned);
}

namespace nxrt::app_heap {
namespace {
constexpr size_t granularity = 2u << 20;
constexpr size_t host_page = 16u << 10;
constexpr size_t minimum_space = 32u << 20;
constexpr size_t max_spaces = 256;
constexpr size_t overhead = 64u << 10;
struct Space { void* base; size_t size; void* handle; };
// Records are immutable once published. Keeping empty spaces for reuse avoids
// racing frees or late process/TLS destructors with an unmap.
constinit Space spaces[max_spaces]{};
constinit std::atomic<size_t> space_count{0}, hint{0}, backing_bytes{0};
constinit std::atomic<size_t> live_blocks{0}, live_bytes{0};
constinit std::atomic_flag growth = ATOMIC_FLAG_INIT;
constinit std::atomic<unsigned> failed_operation{0};
constinit std::atomic<int> platform_error{0};
constinit std::atomic<size_t> failed_bytes{0};
// Bootstrap rollback storage, used only under the growth lock. The bound is
// the console's entire 16 GiB physical capacity, not a physical reservation.
constinit int64_t growing_chunks[(16ull << 30) / granularity]{};
static_assert(std::atomic<size_t>::is_always_lock_free);

struct GrowthLock {
    GrowthLock() noexcept {
        unsigned spins = 0;
        while (growth.test_and_set(std::memory_order_acquire)) {
            if (++spins < 64) __builtin_ia32_pause();
            else sceKernelUsleep(50); // Let a preempted creator make progress.
        }
    }
    ~GrowthLock() { growth.clear(std::memory_order_release); }
};
const Space* owner(const void* pointer) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    const size_t count = space_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const auto& space = spaces[i];
        const uintptr_t base = reinterpret_cast<uintptr_t>(space.base);
        if (address >= base && address - base < space.size) return &space;
    }
    return nullptr;
}
void* take(const Space& space, size_t bytes, size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) return sceLibcMspaceMalloc(space.handle, bytes);
    void* result = nullptr;
    return sceLibcMspacePosixMemalign(space.handle, &result, alignment, bytes) == 0 ? result : nullptr;
}
void* existing(size_t count, size_t bytes, size_t alignment) noexcept {
    if (!count) return nullptr;
    const size_t start = hint.load(std::memory_order_relaxed) % count;
    for (size_t n = 0; n < count; ++n) {
        const size_t index = (start + n) % count;
        if (void* p = take(spaces[index], bytes, alignment)) {
            hint.store(index, std::memory_order_relaxed);
            return p;
        }
    }
    return nullptr;
}
void* grow(size_t bytes, size_t alignment) noexcept {
    GrowthLock lock;
    const size_t count = space_count.load(std::memory_order_acquire);
    // Another allocator may have grown, or a free may have made room.
    if (void* p = existing(count, bytes, alignment)) return p;
    if (count == max_spaces) return nullptr;
    const size_t required = bytes + alignment + overhead;
    const size_t size = required <= minimum_space ? minimum_space
        : (required + granularity - 1) & ~(granularity - 1);
    const int64_t limit = sceKernelGetDirectMemorySize();
    if (limit < 0 || size / granularity > sizeof(growing_chunks) / sizeof(growing_chunks[0]) ||
        (limit > 0 && size > static_cast<uint64_t>(limit))) return nullptr;
    // CPU metadata belongs outside RADV's low graphics window and below the
    // guest's fixed images. A non-fixed reservation never replaces a mapping.
    constexpr uintptr_t first_hint = 0x1000000000ull, upper_bound = 0x4000000000ull;
    const uintptr_t wanted = count ? reinterpret_cast<uintptr_t>(spaces[count - 1].base) + spaces[count - 1].size : first_hint;
    if (size > upper_bound || wanted > upper_bound - size) return nullptr;
    void* base = reinterpret_cast<void*>(wanted);
    int rc = sceKernelReserveVirtualRange(&base, size, 0, granularity);
    if (rc != 0) {
        failed_operation = 5; platform_error = rc; failed_bytes = size;
        return nullptr;
    }
    const uintptr_t actual = reinterpret_cast<uintptr_t>(base);
    if (actual < first_hint || actual > upper_bound - size) {
        munmap(base, size);
        failed_operation = 5; platform_error = EEXIST; failed_bytes = size;
        return nullptr;
    }
    size_t allocated = 0;
    const auto rollback = [&] {
        // If unmapping fails, keep the backing until process teardown.
        if (munmap(base, size) == 0)
            while (allocated) sceKernelReleaseDirectMemory(growing_chunks[--allocated], granularity);
    };
    // Physical memory can be fragmented even with ample total capacity. Match
    // the guest heap's qualified 2 MiB backing units, joined virtually here.
    for (size_t offset = 0; offset < size; offset += granularity) {
        int64_t physical = -1;
        rc = sceKernelAllocateDirectMemory(0, limit, granularity, host_page, 12, &physical);
        if (rc != 0) {
            failed_operation = 1; platform_error = rc; failed_bytes = size;
            rollback();
            return nullptr;
        }
        growing_chunks[allocated++] = physical;
        void* expected = static_cast<unsigned char*>(base) + offset;
        void* at = expected;
        rc = sceKernelMapDirectMemory(&at, granularity, PROT_READ | PROT_WRITE, MAP_FIXED, physical, host_page);
        if (rc != 0 || at != expected) {
            failed_operation = 2; platform_error = rc; failed_bytes = size;
            rollback();
            return nullptr;
        }
    }
    std::memset(base, 0, size);
    void* handle = sceLibcMspaceCreate("Pokopia-app", base, size, 0);
    if (!handle) {
        failed_operation = 3; platform_error = errno; failed_bytes = size;
        // Never release physical memory beneath a mapping that could not close.
        rollback();
        return nullptr;
    }
    spaces[count] = {base, size, handle};
    backing_bytes.fetch_add(size, std::memory_order_relaxed);
    space_count.store(count + 1, std::memory_order_release);
    hint.store(count, std::memory_order_relaxed);
    void* result = take(spaces[count], bytes, alignment);
    if (!result) { failed_operation = 4; platform_error = errno; failed_bytes = bytes; }
    return result;
}
void* allocate(size_t bytes, size_t alignment) noexcept {
    if (!bytes) bytes = 1;
    constexpr size_t maximum = std::numeric_limits<size_t>::max();
    if (alignment > maximum - overhead - granularity ||
        bytes > maximum - alignment - overhead - granularity) {
        errno = ENOMEM;
        return nullptr;
    }
    void* result = existing(space_count.load(std::memory_order_acquire), bytes, alignment);
    if (!result) result = grow(bytes, alignment);
    if (result) {
        live_bytes.fetch_add(sceLibcMspaceMallocUsableSize(result), std::memory_order_relaxed);
        live_blocks.fetch_add(1, std::memory_order_relaxed);
    } else errno = ENOMEM;
    return result;
}
}
bool owns(const void* p) noexcept { return owner(p) != nullptr; }
Stats stats() noexcept {
    return {space_count.load(std::memory_order_acquire), backing_bytes.load(std::memory_order_relaxed),
            live_blocks.load(std::memory_order_relaxed), live_bytes.load(std::memory_order_relaxed),
            failed_operation.load(), platform_error.load(), failed_bytes.load()};
}
}

extern "C" void* __wrap_malloc(size_t bytes) {
    return nxrt::app_heap::allocate(bytes, alignof(std::max_align_t));
}
extern "C" void __wrap_free(void* p) {
    if (!p) return;
    if (const auto* space = nxrt::app_heap::owner(p)) {
        nxrt::app_heap::live_bytes.fetch_sub(sceLibcMspaceMallocUsableSize(p), std::memory_order_relaxed);
        nxrt::app_heap::live_blocks.fetch_sub(1, std::memory_order_relaxed);
        sceLibcMspaceFree(space->handle, p);
    } else __real_free(p); // System libraries retain ownership of their allocations.
}
extern "C" void* __wrap_calloc(size_t count, size_t bytes) {
    if (bytes && count > std::numeric_limits<size_t>::max() / bytes) { errno = ENOMEM; return nullptr; }
    const size_t size = count * bytes;
    void* p = __wrap_malloc(size);
    if (p) std::memset(p, 0, size);
    return p;
}
extern "C" void* __wrap_realloc(void* p, size_t bytes) {
    if (!p) return __wrap_malloc(bytes);
    if (!bytes) { __wrap_free(p); return nullptr; }
    if (!nxrt::app_heap::owner(p)) return __real_realloc(p, bytes);
    const size_t old_size = sceLibcMspaceMallocUsableSize(p);
    if (bytes <= old_size) return p;
    void* result = __wrap_malloc(bytes);
    if (result) { std::memcpy(result, p, old_size); __wrap_free(p); }
    return result; // A failed growth leaves the original allocation intact.
}
extern "C" int __wrap_posix_memalign(void** output, size_t alignment, size_t bytes) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1))) return EINVAL;
    const int saved_errno = errno;
    void* p = nxrt::app_heap::allocate(bytes, alignment);
    errno = saved_errno;
    if (!p) return ENOMEM;
    *output = p;
    return 0;
}
extern "C" void* __wrap_aligned_alloc(size_t alignment, size_t bytes) {
    if (!alignment || (alignment & (alignment - 1)) || bytes % alignment) { errno = EINVAL; return nullptr; }
    return nxrt::app_heap::allocate(bytes, alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment);
}
extern "C" void* __wrap_memalign(size_t alignment, size_t bytes) {
    if (!alignment || (alignment & (alignment - 1))) { errno = EINVAL; return nullptr; }
    return nxrt::app_heap::allocate(bytes, alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment);
}
extern "C" size_t __wrap_malloc_usable_size(const void* p) {
    if (!p) return 0;
    return nxrt::app_heap::owner(p) ? sceLibcMspaceMallocUsableSize(p) : __real_malloc_usable_size(p);
}
