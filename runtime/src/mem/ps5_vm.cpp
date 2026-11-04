// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/ps5_vm.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sys/mman.h>
#include <utility>

extern "C" {
int sceKernelReserveVirtualRange(void**, size_t, int, size_t);
int64_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
}

namespace nxrt::ps5_vm {
namespace {
bool aligned(size_t n) { const size_t p = host_vm::page_size(); return p && n % p == 0; }
bool within(size_t offset, size_t size, size_t limit) {
    return size && aligned(offset) && aligned(size) && offset <= limit && size <= limit - offset;
}
bool error(int rc) {
    if (!rc) return false;
    const auto code = static_cast<unsigned>(rc);
    errno = (code & 0xffff0000u) == 0x80020000u ? static_cast<int>(code & 0xffffu) : EIO;
    return true;
}
int permissions(host_vm::Access access) {
    switch (access) {
        case host_vm::Access::None: return PROT_NONE;
        case host_vm::Access::Read: return PROT_READ;
        case host_vm::Access::ReadWrite: return PROT_READ | PROT_WRITE;
    }
    return -1;
}
}
struct Backing {
    int64_t physical = -1;
    size_t size = 0;
    ~Backing() {
        if (physical >= 0 && error(sceKernelReleaseDirectMemory(physical, size)))
            std::perror("ps5_vm: release direct memory");
    }
};

DirectMemory DirectMemory::allocate(size_t size) {
    DirectMemory result;
    if (!size || !aligned(size)) { errno = EINVAL; return result; }
    auto backing = std::make_shared<Backing>();
    const int64_t limit = sceKernelGetDirectMemorySize();
    // This PS5 reports zero here while allocations with end=0 succeed. Keep
    // that native API value instead of interpreting it as an empty pool.
    if (limit < 0 || (limit > 0 && size > static_cast<uint64_t>(limit))) {
        std::fprintf(stderr, "ps5_vm: direct size=%zu limit=%lld\n", size, static_cast<long long>(limit));
        errno = ENOMEM;
        return result;
    }
    int64_t physical = -1;
    const int rc = sceKernelAllocateDirectMemory(0, limit, size, host_vm::page_size(), 12, &physical);
    if (error(rc)) {
        std::fprintf(stderr, "ps5_vm: allocate size=%zu align=%zu limit=%lld rc=%08x\n",
                     size, host_vm::page_size(), static_cast<long long>(limit), static_cast<unsigned>(rc));
        return result;
    }
    backing->physical = physical;
    backing->size = size;
    result.backing_ = std::move(backing);
    return result;
}
size_t DirectMemory::size() const { return backing_ ? backing_->size : 0; }

Reservation::~Reservation() {
    if (!reset()) {
        // Do not release physical backing while a failed unmap leaves views
        // live. Process teardown is safer than reusing that physical memory.
        std::perror("ps5_vm: release reservation");
        std::abort();
    }
}
Reservation::Reservation(Reservation&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), size_(std::exchange(other.size_, 0)),
      views_(std::move(other.views_)) {}
Reservation Reservation::reserve(size_t size, uintptr_t address) {
    Reservation result;
    if (!size || !aligned(size) || !aligned(address) || size > std::numeric_limits<uintptr_t>::max() - address) {
        errno = EINVAL;
        return result;
    }
    void* at = reinterpret_cast<void*>(address);
    if (error(sceKernelReserveVirtualRange(&at, size, 0, host_vm::page_size()))) return result;
    if (address && reinterpret_cast<uintptr_t>(at) != address) {
        if (munmap(at, size) != 0) std::perror("ps5_vm: release alternative reservation");
        errno = EEXIST;
        return result;
    }
    result.address_ = at;
    result.size_ = size;
    return result;
}
bool Reservation::map(size_t offset, const DirectMemory& memory, size_t backing_offset,
                      size_t size, host_vm::Access access) {
    const int prot = permissions(access);
    if (!within(offset, size, size_) || !within(backing_offset, size, memory.size()) || prot < 0) {
        errno = EINVAL;
        return false;
    }
    for (const auto& view : views_) {
        if (offset < view.offset + view.size && view.offset < offset + size) {
            errno = EEXIST;
            return false;
        }
    }
    // Allocate bookkeeping before changing the mapping so exceptions cannot
    // leave an untracked view. All MAP_FIXED calls stay inside owned ranges.
    views_.push_back(View{offset, size, memory.backing_});
    void* expected = static_cast<unsigned char*>(address_) + offset;
    void* at = expected;
    if (error(sceKernelMapDirectMemory(&at, size, prot, MAP_FIXED,
              memory.backing_->physical + static_cast<int64_t>(backing_offset), host_vm::page_size()))) {
        views_.pop_back();
        return false;
    }
    if (at != expected) std::abort(); // Native fixed-map contract violation.
    std::sort(views_.begin(), views_.end(), [](const View& a, const View& b) { return a.offset < b.offset; });
    return true;
}
bool Reservation::protect(size_t offset, size_t size, host_vm::Access access) {
    const int prot = permissions(access);
    if (!within(offset, size, size_) || prot < 0) { errno = EINVAL; return false; }
    size_t covered = offset;
    for (const auto& view : views_) {
        if (view.offset > covered) break;
        if (view.offset + view.size > covered) covered = view.offset + view.size;
        if (covered >= offset + size)
            return mprotect(static_cast<unsigned char*>(address_) + offset, size, prot) == 0;
    }
    errno = ENOMEM;
    return false;
}
bool Reservation::reset() {
    if (size_ && munmap(address_, size_) != 0) return false;
    address_ = nullptr;
    size_ = 0;
    views_.clear(); // Release backing only after all of this reservation's views.
    return true;
}
bool Reservation::discard(size_t offset, size_t size) {
    if (!within(offset, size, size_)) { errno = EINVAL; return false; }
    std::vector<View> remaining;
    remaining.reserve(views_.size() + 1);
    for (const auto& view : views_) {
        const size_t end = view.offset + view.size;
        if (view.offset >= offset + size || end <= offset) remaining.push_back(view);
        else {
            if (view.offset < offset) remaining.push_back({view.offset, offset - view.offset, view.backing});
            if (end > offset + size) remaining.push_back({offset + size, end - offset - size, view.backing});
        }
    }
    void* expected = static_cast<unsigned char*>(address_) + offset;
    void* at = expected;
    if (error(sceKernelReserveVirtualRange(&at, size, MAP_FIXED, host_vm::page_size()))) return false;
    if (at != expected) std::abort();
    views_.swap(remaining); // Last references drop only after the views are gone.
    return true;
}
} // namespace nxrt::ps5_vm
