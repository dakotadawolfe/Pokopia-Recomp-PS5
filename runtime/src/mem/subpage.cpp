// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/subpage.h"
#include "nxrecomp/memory.h"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

extern "C" {
alignas(64) uint8_t nx_guest_subpage_flags[NX_GUEST_ADDRESS_END >> NX_GUEST_HOST_PAGE_SHIFT]{};
}
namespace nxrt::mem {
namespace {
constexpr uint64_t guest_page = 4096, host_page = 16384;
std::mutex registry_mutex;
std::map<uint64_t, SubpageMap*> registry;
host_vm::Access host_access(unsigned bits) {
    return bits & 2 ? host_vm::Access::ReadWrite : bits & 1 ? host_vm::Access::Read : host_vm::Access::None;
}
void flag(uint64_t address, bool mixed) {
    __atomic_store_n(nx_guest_subpage_flags + (address >> NX_GUEST_HOST_PAGE_SHIFT),
                     static_cast<uint8_t>(mixed), __ATOMIC_RELEASE);
}
}

SubpageMap::SubpageMap(uint64_t base, uint64_t size, Apply apply)
    : base_(base), size_(size), apply_(std::move(apply)), permissions_(size / guest_page, 0) {}
std::unique_ptr<SubpageMap> SubpageMap::create(uint64_t base, uint64_t size, Apply apply) {
    if (!size || base % host_page || size % host_page || base >= NX_GUEST_ADDRESS_END ||
        size > NX_GUEST_ADDRESS_END - base || !apply) return {};
    std::lock_guard lock(registry_mutex);
    auto it = registry.lower_bound(base);
    if (it != registry.end() && it->first < base + size) return {};
    if (it != registry.begin()) {
        auto prev = std::prev(it)->second;
        if (prev->base_ + prev->size_ > base) return {};
    }
    auto result = std::unique_ptr<SubpageMap>(new SubpageMap(base, size, std::move(apply)));
    registry.emplace(base, result.get());
    result->registered_ = true;
    return result;
}
SubpageMap::~SubpageMap() {
    if (!registered_) return;
    std::lock_guard lock(registry_mutex);
    registry.erase(base_);
    for (uint64_t a = base_; a < base_ + size_; a += host_page) flag(a, false);
}

bool SubpageMap::protect(uint64_t address, uint64_t size, unsigned access) {
    if (!size || address % guest_page || size % guest_page || (access != 0 && access != 1 && access != 3) ||
        address < base_ || address - base_ > size_ || size > size_ - (address - base_)) return false;
    std::lock_guard lock(registry_mutex);
    const uint64_t first = (address - base_) / guest_page, end = first + size / guest_page;
    const uint64_t block_first = first / 4, block_end = (end + 3) / 4;
    struct Block { unsigned before, after; bool mixed_before, mixed_after; };
    struct Run { uint64_t first, count; unsigned before, after; };
    std::vector<Block> blocks;
    std::vector<Run> runs;
    blocks.reserve(block_end - block_first);
    for (uint64_t b = block_first; b < block_end; ++b) {
        unsigned before = 0, after = 0;
        unsigned first_before = permissions_[b * 4], first_after = b * 4 >= first && b * 4 < end ? access : first_before;
        bool mixed_before = false, mixed_after = false;
        for (uint64_t p = b * 4; p < b * 4 + 4; ++p) {
            const unsigned old = permissions_[p], next = p >= first && p < end ? access : old;
            before |= old; after |= next;
            mixed_before |= old != first_before; mixed_after |= next != first_after;
        }
        blocks.push_back({before, after, mixed_before, mixed_after});
        if (before != after) {
            if (!runs.empty() && runs.back().first + runs.back().count == b &&
                runs.back().before == before && runs.back().after == after) ++runs.back().count;
            else runs.push_back({b, 1, before, after});
        }
    }
    // Force affected guest accesses through the locked slow path while the
    // host permissions and guest metadata change. No unrelated block changes.
    for (uint64_t b = block_first; b < block_end; ++b) flag(base_ + b * host_page, true);
    size_t applied = 0;
    for (; applied < runs.size(); ++applied) {
        const auto& r = runs[applied];
        bool ok = false;
        try { ok = apply_(base_ + r.first * host_page, r.count * host_page, host_access(r.after)); }
        catch (...) { /* Restore prior permissions below. */ }
        if (!ok) break;
    }
    if (applied != runs.size()) {
        while (applied) {
            const auto& r = runs[--applied];
            bool restored = false;
            try { restored = apply_(base_ + r.first * host_page, r.count * host_page, host_access(r.before)); }
            catch (...) { /* A failed rollback cannot leave executable guest threads. */ }
            if (!restored) {
                std::fputs("subpage: host protection rollback failed\n", stderr);
                std::abort();
            }
        }
        for (uint64_t b = block_first; b < block_end; ++b) flag(base_ + b * host_page, blocks[b - block_first].mixed_before);
        return false;
    }
    std::fill(permissions_.begin() + first, permissions_.begin() + end, static_cast<uint8_t>(access));
    for (uint64_t b = block_first; b < block_end; ++b) flag(base_ + b * host_page, blocks[b - block_first].mixed_after);
    return true;
}
bool SubpageMap::allows(uint64_t address, uint64_t size, unsigned access) {
    if (!size) return true;
    if (!access || access > 3 || address >= NX_GUEST_ADDRESS_END || size > NX_GUEST_ADDRESS_END - address) return false;
    std::lock_guard lock(registry_mutex);
    const uint64_t end = address + size;
    for (uint64_t at = address; at < end;) {
        auto it = registry.upper_bound(at);
        if (it == registry.begin()) return false;
        const auto* range = std::prev(it)->second;
        if (at < range->base_ || at - range->base_ >= range->size_) return false;
        const uint64_t stop = std::min(end, range->base_ + range->size_);
        for (uint64_t p = (at - range->base_) / guest_page; p < (stop - range->base_ + guest_page - 1) / guest_page; ++p)
            if ((range->permissions_[p] & access) != access) return false;
        at = stop;
    }
    return true;
}
} // namespace nxrt::mem
extern "C" void nx_guest_subpage_slow(uint64_t address, size_t size, unsigned access) {
    if (nxrt::mem::SubpageMap::allows(address, size, access)) return;
    // No lock or C++ lifetime crosses the signal boundary. The crash handler
    // can report the guest fault; a returning handler must never allow access.
    std::fprintf(stderr, "guest permission fault: address=%llx size=%zu access=%u\n",
                 static_cast<unsigned long long>(address), size, access);
    std::raise(SIGSEGV);
    std::abort();
}
