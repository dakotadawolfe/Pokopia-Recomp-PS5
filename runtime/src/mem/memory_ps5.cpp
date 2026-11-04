// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-memory API backed by PS5 reservations and shared direct memory.
#include "nxrt/memory.h"
#include "nxrt/guest_region.h"
#include "nxrt/shared_heap.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrecomp/memory.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

namespace nxrt::mem {
namespace {
constexpr uint64_t limit = NX_GUEST_ADDRESS_END;
struct Fixed { uint64_t size; std::unique_ptr<GuestRegion> owner; };
std::mutex mutex;
std::map<uint64_t, Fixed> fixed;
std::map<uint64_t, VmInfo> vm;
std::unique_ptr<SharedHeap> heap;
bool ready = false;
uint64_t default_heap = 0, kernel_next = kKernelBase;

uint64_t up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
bool span(uint64_t address, uint64_t size, uint64_t& first, uint64_t& end) {
    if (address > limit || size > limit - address) return false;
    first = address & ~(kPage - 1);
    end = size ? up(address + size, kPage) : first;
    return true;
}
bool valid(Prot p) {
    return p == Prot::None || p == Prot::R || p == Prot::RW || p == Prot::RX || p == Prot::RWX;
}
bool reserve(uint64_t address, uint64_t size, const char* tag) {
    if (!address || address % kAllocGranularity || !size || address >= limit || size > limit - address) return false;
    size = up(size, kPage);
    auto owner = GuestRegion::create(address, size);
    if (!owner) return false;
    fixed.emplace(address, Fixed{size, std::move(owner)});
    NXLOG_D("mem", "reserved %llx (+%llx) %s", static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size), tag ? tag : "");
    return true;
}
bool release(uint64_t address) {
    auto it = fixed.find(address);
    if (it == fixed.end() || !it->second.owner->reset()) return false;
    fixed.erase(it); return true;
}
struct Part {
    uint64_t address, size;
    GuestRegion* region;
    bool committed() const { return region ? region->is_committed(address, size) : heap->is_committed(address, size); }
    bool accessible() const { return region ? region->is_mapped(address, size) : heap->is_mapped(address, size); }
    bool protect(Prot p) const { return region ? region->protect(address, size, p) : heap->protect(address, size, p); }
};
bool parts(uint64_t first, uint64_t end, std::vector<Part>& out) {
    while (first < end) {
        auto it = fixed.upper_bound(first);
        if (it != fixed.begin()) {
            --it;
            if (first < it->first + it->second.size) {
                const auto stop = std::min(end, it->first + it->second.size);
                out.push_back({first, stop - first, it->second.owner.get()}); first = stop; continue;
            }
        }
        const uint64_t heap_end = heap ? heap->view_end(first) : 0;
        if (!heap_end) return false;
        const auto stop = std::min(end, heap_end);
        out.push_back({first, stop - first, nullptr}); first = stop;
    }
    return true;
}
bool commit_locked(uint64_t address, uint64_t size, Prot p) {
    uint64_t first, end;
    if (!valid(p) || !span(address, size, first, end)) return false;
    if (first == end) return true;
    std::vector<Part> ranges;
    if (!parts(first, end, ranges) || ranges.size() != 1) return false;
    auto& range = ranges.front();
    return range.region ? range.region->commit(first, end - first, p) : heap->commit_at(first, end - first, p);
}
struct Gauge {
    const char* name;
    Gauge(const char* n, std::function<int64_t()> fn) : name(n) { gauge_register(n, std::move(fn)); }
    ~Gauge() { gauge_unregister(name); }
};
Gauge reserved_gauge("mem.heap_reserved", [] { return static_cast<int64_t>(heap_reserved()); });
Gauge committed_gauge("mem.heap_committed", [] { return static_cast<int64_t>(heap_committed()); });
Gauge aliases_gauge("mem.aliases", [] { return static_cast<int64_t>(heap_alias_count()); });
Gauge fixed_gauge("mem.fixed_regions", [] { std::lock_guard lock(mutex); return static_cast<int64_t>(fixed.size()); });
Gauge vm_gauge("mem.vm_blocks", [] { std::lock_guard lock(mutex); return static_cast<int64_t>(vm.size()); });
}

bool init(const Config& config) {
    std::lock_guard lock(mutex);
    if (ready) return true;
    uint64_t bytes = config.heap_reserve;
    if (!bytes) {
        auto gb = config_get_int("memory", "heap_gb", game::info().platform == game::Platform::Switch2 ? 12 : 6);
        if (gb < 1 || gb > 12) gb = 6;
        bytes = static_cast<uint64_t>(gb) << 30;
    }
    if (bytes > kHeapMax) return false;
    if (config.reserve_regions) {
        if (!reserve(kStackBase, kStackSize, "stack")) return false;
        if (!reserve(kKernelBase, kKernelSize, "kernel")) {
            if (!release(kStackBase)) std::abort();
            return false;
        }
    }
    default_heap = up(bytes, kAllocGranularity); kernel_next = kKernelBase; ready = true;
    return true;
}
void shutdown() {
    std::lock_guard lock(mutex);
    if (heap && !heap->reset()) std::abort();
    heap.reset();
    while (!fixed.empty()) if (!release(fixed.begin()->first)) std::abort();
    vm.clear(); default_heap = 0; kernel_next = kKernelBase; ready = false;
}
bool initialised() { std::lock_guard lock(mutex); return ready; }
bool reserve_fixed(uint64_t address, uint64_t size, const char* tag) {
    std::lock_guard lock(mutex); return reserve(address, size, tag);
}
bool release_fixed(uint64_t address) { std::lock_guard lock(mutex); return release(address); }
bool commit(uint64_t address, uint64_t size, Prot p) { std::lock_guard lock(mutex); return commit_locked(address, size, p); }
bool decommit(uint64_t address, uint64_t size) {
    std::lock_guard lock(mutex);
    uint64_t first, end;
    if (!span(address, size, first, end)) return false;
    if (first == end) return true;
    std::vector<Part> ranges;
    // Section views use heap_decommit; generic decommit releases private pages.
    return parts(first, end, ranges) && ranges.size() == 1 && ranges[0].region && ranges[0].region->decommit(first, end - first);
}
bool protect(uint64_t address, uint64_t size, Prot p) {
    std::lock_guard lock(mutex);
    uint64_t first, end;
    if (!valid(p) || !span(address, size, first, end)) return false;
    if (first == end) return true;
    std::vector<Part> ranges;
    if (!parts(first, end, ranges)) return false;
    if (ranges.size() == 1) return ranges.front().protect(p);
    // Cross-reservation changes preflight every page, then retain enough state
    // to undo earlier reservations if a later native operation fails.
    struct Undo { Part part; Prot access; };
    std::vector<std::vector<Undo>> undo(ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& r = ranges[i];
        if (!r.committed()) return false;
        for (uint64_t at = r.address; at < r.address + r.size; at += kPage) {
            const Prot access = SubpageMap::allows(at, kPage, 3) ? Prot::RW :
                                SubpageMap::allows(at, kPage, 1) ? Prot::R : Prot::None;
            auto& list = undo[i];
            if (!list.empty() && list.back().access == access) list.back().part.size += kPage;
            else list.push_back({{at, kPage, r.region}, access});
        }
    }
    size_t done = 0;
    auto rollback = [&] {
        while (done) for (const auto& item : undo[--done]) {
            bool restored = false;
            try { restored = item.part.protect(item.access); } catch (...) {}
            if (!restored) std::abort();
        }
    };
    try {
        for (; done < ranges.size(); ++done)
            if (!ranges[done].protect(p)) { rollback(); return false; }
    } catch (...) { rollback(); throw; }
    return true;
}
bool is_mapped(uint64_t address, uint64_t size) {
    std::lock_guard lock(mutex);
    uint64_t first, end;
    if (!span(address, size, first, end)) return false;
    std::vector<Part> ranges;
    if (!parts(first, end, ranges)) return false;
    return std::all_of(ranges.begin(), ranges.end(), [](const Part& r) { return r.accessible(); });
}
uint64_t alloc_kernel(uint64_t size, uint64_t alignment) {
    if (!initialised() && !init()) return 0;
    if (!alignment) alignment = kPage;
    if ((alignment & (alignment - 1)) || alignment > limit || size > kKernelSize) return 0;
    alignment = std::max(alignment, kPage); size = up(std::max(size, uint64_t{1}), kPage);
    std::lock_guard lock(mutex);
    const auto at = up(kernel_next, alignment);
    if (at < kernel_next || at > kKernelBase + kKernelSize || size > kKernelBase + kKernelSize - at) return 0;
    if (!commit_locked(at, size, Prot::RW)) return 0;
    kernel_next = at + size; return at;
}
bool heap_create(uint64_t bytes) {
    if (!initialised() && !init()) return false;
    std::lock_guard lock(mutex);
    if (heap) return true;
    if (!ready) return false;
    heap = SharedHeap::create(bytes ? bytes : default_heap);
    return static_cast<bool>(heap);
}
bool heap_exists() { std::lock_guard lock(mutex); return static_cast<bool>(heap); }
uint64_t heap_reserved() { std::lock_guard lock(mutex); return heap ? heap->reserved_bytes() : 0; }
uint64_t heap_committed() { std::lock_guard lock(mutex); return heap ? heap->committed_bytes() : 0; }
bool heap_commit(uint64_t offset, uint64_t size) { std::lock_guard lock(mutex); return heap && heap->commit(offset, size); }
bool heap_decommit(uint64_t offset, uint64_t size) { std::lock_guard lock(mutex); return heap && heap->decommit(offset, size); }
uint64_t heap_alias(uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    std::lock_guard lock(mutex); return heap ? heap->alias(offset, size, at, p) : 0;
}
bool heap_alias_unmap(uint64_t at) { std::lock_guard lock(mutex); return heap && heap->unmap_alias(at); }
void* heap_private_view(uint64_t offset, uint64_t size) { std::lock_guard lock(mutex); return heap ? heap->private_view(offset, size) : nullptr; }
bool heap_private_view_unmap(void* at) { std::lock_guard lock(mutex); return heap && heap->unmap_private(at); }
size_t heap_alias_count() { std::lock_guard lock(mutex); return heap ? heap->alias_count() : 0; }
void heap_alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) { std::lock_guard lock(mutex); if (heap) heap->alias_ranges(out); }
bool heap_offset_of(uint64_t at, uint64_t* offset) { std::lock_guard lock(mutex); return heap && heap->offset_of(at, offset); }

void vm_set(uint64_t address, uint64_t size, uint32_t state, uint32_t perm) {
    if (!size || address >= limit || size > limit - address) return;
    const uint64_t end = address + size;
    std::lock_guard lock(mutex);
    auto it = vm.lower_bound(address);
    if (it != vm.begin()) {
        auto previous = std::prev(it);
        if (previous->second.addr + previous->second.size > address) it = previous;
    }
    while (it != vm.end() && it->second.addr < end) {
        const auto block = it->second; it = vm.erase(it);
        if (block.addr < address) vm.emplace(block.addr, VmInfo{block.addr, address - block.addr, block.state, block.perm});
        if (block.addr + block.size > end) vm.emplace(end, VmInfo{end, block.addr + block.size - end, block.state, block.perm});
    }
    if (state != kMemFree) vm.emplace(address, VmInfo{address, size, state, perm});
}
VmInfo vm_query(uint64_t address) {
    std::lock_guard lock(mutex);
    if (address >= limit) return {limit, 0, kMemInaccessible, 0};
    auto it = vm.upper_bound(address);
    uint64_t start = 0;
    if (it != vm.begin()) {
        const auto& previous = std::prev(it)->second;
        if (address < previous.addr + previous.size) return previous;
        start = previous.addr + previous.size;
    }
    const uint64_t end = it == vm.end() ? limit : it->first;
    return {start, end - start, kMemFree, 0};
}
} // namespace nxrt::mem
