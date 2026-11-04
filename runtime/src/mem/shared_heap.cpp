// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/shared_heap.h"
#include "nxrecomp/memory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nxrt::mem {
namespace {
constexpr uint64_t host_page = 16384, block_size = 2 * 1024 * 1024;
uint64_t up(uint64_t n, uint64_t alignment) { return (n + alignment - 1) & ~(alignment - 1); }
bool span(uint64_t offset, uint64_t bytes, uint64_t limit, uint64_t& first, uint64_t& end) {
    if (offset > limit || bytes > limit - offset) return false;
    first = offset / kPage; end = bytes ? up(offset + bytes, kPage) / kPage : first;
    return true;
}
unsigned bits(Prot p) {
    switch (p) {
        case Prot::None: return 0;
        case Prot::R: case Prot::RX: return 1;
        case Prot::RW: case Prot::RWX: return 3;
    }
    return 4;
}
}
struct SharedHeap::View {
    uint64_t offset, size, base;
    unsigned initial;
    ps5_vm::Reservation mapping;
    std::unique_ptr<SubpageMap> policy;
    std::vector<uint8_t> permissions;
    View(uint64_t off, uint64_t bytes, unsigned access, ps5_vm::Reservation reservation)
        : offset(off), size(bytes), base(reinterpret_cast<uint64_t>(reservation.data())), initial(access),
          mapping(std::move(reservation)), permissions(mapping.size() / kPage, 0) {}
    bool reset() {
        if (!mapping.reset()) return false;
        policy.reset(); return true;
    }
    ~View() { if (!reset()) { std::perror("shared heap: release view"); std::abort(); } }
};
struct SharedHeap::Change { View* view; uint64_t page, count; unsigned before, after; };
struct SharedHeap::Binding { View* view; uint64_t offset, size; };

SharedHeap::SharedHeap(uint64_t bytes)
    : size_(bytes), blocks_(up(bytes, block_size) / block_size), available_(bytes / kPage, 0), active_(bytes / kPage, 0) {}
std::unique_ptr<SharedHeap> SharedHeap::create(uint64_t bytes) {
    if (!bytes || bytes > kHeapMax) return {};
    bytes = up(bytes, kAllocGranularity);
    auto heap = std::unique_ptr<SharedHeap>(new SharedHeap(bytes));
    heap->primary_ = heap->make_view(0, bytes, kHeapBase, 3, false);
    if (!heap->primary_) return {};
    return heap;
}
SharedHeap::~SharedHeap() { if (!reset()) { std::perror("shared heap: release"); std::abort(); } }

void SharedHeap::append(std::vector<Change>& changes, View& view, uint64_t page, unsigned access) {
    const unsigned previous = view.permissions[page];
    if (previous == access) return;
    if (!changes.empty()) {
        auto& last = changes.back();
        if (last.view == &view && last.page + last.count == page && last.before == previous && last.after == access) {
            ++last.count; return;
        }
    }
    changes.push_back({&view, page, 1, previous, access});
}
bool SharedHeap::apply(std::vector<Change>& changes) {
    size_t done = 0;
    auto rollback = [&] {
        while (done) {
            const auto& c = changes[--done];
            bool restored = false;
            try { restored = c.view->policy->protect(c.view->base + c.page * kPage, c.count * kPage, c.before); }
            catch (...) { /* Failed rollback cannot leave guest accesses running. */ }
            if (!restored) {
                std::perror("shared heap: permission rollback"); std::abort();
            }
        }
    };
    try {
        for (; done < changes.size(); ++done) {
            const auto& c = changes[done];
            if (!c.view->policy->protect(c.view->base + c.page * kPage, c.count * kPage, c.after)) {
                rollback(); return false;
            }
        }
    } catch (...) { rollback(); throw; }
    for (const auto& c : changes)
        std::fill_n(c.view->permissions.begin() + c.page, c.count, static_cast<uint8_t>(c.after));
    return true;
}
bool SharedHeap::attach(View& view, size_t index, const Block& backing, std::vector<Binding>* added) {
    const uint64_t first = std::max(view.offset, index * block_size);
    const uint64_t end = std::min(view.offset + view.mapping.size(), index * block_size + backing.size);
    if (first >= end) return true;
    if (!view.mapping.map(first - view.offset, backing.memory, first - index * block_size,
                          end - first, host_vm::Access::None)) return false;
    if (added) added->push_back({&view, first - view.offset, end - first});
    return true;
}
void SharedHeap::rollback_backing(const std::vector<Binding>& bindings, const std::vector<size_t>& blocks) {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
        if (!it->view->mapping.discard(it->offset, it->size)) {
            std::perror("shared heap: backing rollback"); std::abort();
        }
    for (size_t index : blocks) blocks_[index].reset();
}

std::unique_ptr<SharedHeap::View> SharedHeap::make_view(uint64_t offset, uint64_t bytes, uint64_t at,
                                                       unsigned access, bool internal) {
    auto mapping = ps5_vm::Reservation::reserve(up(bytes, host_page), at);
    if (!mapping) return {};
    auto view = std::make_unique<View>(offset, up(bytes, kPage), access, std::move(mapping));
    auto* owner = view.get();
    auto protect = [owner](uint64_t address, uint64_t size, host_vm::Access permissions) {
        return owner->mapping.protect(address - owner->base, size, permissions);
    };
    view->policy = internal ? SubpageMap::create_private(view->base, view->mapping.size(), protect)
                            : SubpageMap::create(view->base, view->mapping.size(), protect);
    if (!view->policy) return {};
    const size_t first = offset / block_size, end = up(offset + bytes, block_size) / block_size;
    for (size_t index = first; index < end; ++index)
        if (blocks_[index] && !attach(*view, index, *blocks_[index])) return {};
    std::vector<Change> changes;
    for (uint64_t page = 0; page < view->size / kPage; ++page)
        if (available_[offset / kPage + page]) append(changes, *view, page, access);
    if (!apply(changes)) return {};
    return view;
}
bool SharedHeap::commit_view(View& target, uint64_t offset, uint64_t bytes, unsigned access, bool count) {
    uint64_t first, end;
    if (access > 3 || !span(offset, bytes, target.size, first, end)) return false;
    if (first == end) return true;
    first += target.offset / kPage; end += target.offset / kPage;
    std::vector<View*> views{primary_.get()};
    for (auto& [address, view] : aliases_) { (void)address; views.push_back(view.get()); }
    for (auto& [address, view] : private_) { (void)address; views.push_back(view.get()); }
    std::vector<Change> changes;
    for (auto* view : views) {
        const uint64_t begin = std::max(first, view->offset / kPage);
        const uint64_t stop = std::min(end, (view->offset + view->size) / kPage);
        for (uint64_t page = begin; page < stop; ++page) {
            if (view == &target) append(changes, *view, page - view->offset / kPage, access);
            else if (!available_[page]) append(changes, *view, page - view->offset / kPage, view->initial);
        }
    }
    const size_t first_block = first * kPage / block_size, end_block = up(end * kPage, block_size) / block_size;
    std::vector<size_t> added;
    std::vector<Binding> bindings;
    added.reserve(end_block - first_block);
    if (views.size() > SIZE_MAX / (end_block - first_block)) return false;
    bindings.reserve((end_block - first_block) * views.size());
    try {
        for (size_t index = first_block; index < end_block; ++index) {
            if (blocks_[index]) continue;
            auto block = std::make_unique<Block>();
            block->size = std::min(block_size, size_ - index * block_size);
            block->memory = ps5_vm::DirectMemory::allocate(block->size);
            auto writable = ps5_vm::Reservation::reserve(block->size);
            if (!block->memory || !writable || !writable.map(0, block->memory, 0, block->size, host_vm::Access::ReadWrite)) {
                rollback_backing(bindings, added); return false;
            }
            std::memset(writable.data(), 0, block->size);
            blocks_[index] = std::move(block); added.push_back(index);
            for (auto* view : views)
                if (!attach(*view, index, *blocks_[index], &bindings)) { rollback_backing(bindings, added); return false; }
        }
        if (!apply(changes)) { rollback_backing(bindings, added); return false; }
    } catch (...) { rollback_backing(bindings, added); throw; }
    for (uint64_t page = first; page < end; ++page) {
        available_[page] = 1;
        if (count && !active_[page]) { active_[page] = 1; committed_ += kPage; }
    }
    return true;
}
bool SharedHeap::commit(uint64_t offset, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    return primary_ && commit_view(*primary_, offset, bytes, 3, true);
}
SharedHeap::View* SharedHeap::find_view(uint64_t address, uint64_t bytes) const {
    if (!primary_) return nullptr;
    auto within = [=](const View& v) { return address >= v.base && address - v.base <= v.size && bytes <= v.size - (address - v.base); };
    if (within(*primary_)) return primary_.get();
    auto it = aliases_.upper_bound(address);
    if (it != aliases_.begin()) { --it; if (within(*it->second)) return it->second.get(); }
    return nullptr;
}
bool SharedHeap::commit_at(uint64_t address, uint64_t bytes, Prot protection) {
    std::lock_guard lock(mutex_);
    auto* view = find_view(address, bytes);
    return view && commit_view(*view, address - view->base, bytes, bits(protection), false);
}
bool SharedHeap::decommit(uint64_t offset, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    uint64_t first, end;
    if (!primary_ || !span(offset, bytes, size_, first, end)) return false;
    std::vector<Change> changes;
    for (uint64_t page = first; page < end; ++page) if (active_[page]) append(changes, *primary_, page, 0);
    if (!apply(changes)) return false;
    for (uint64_t page = first; page < end; ++page)
        if (active_[page]) { active_[page] = 0; committed_ -= kPage; }
    return true;
}
bool SharedHeap::protect(uint64_t address, uint64_t bytes, Prot protection) {
    std::lock_guard lock(mutex_);
    auto* view = find_view(address, bytes);
    uint64_t first, end;
    const unsigned access = bits(protection);
    if (!view || access > 3 || !span(address - view->base, bytes, view->size, first, end)) return false;
    for (uint64_t page = first; page < end; ++page) if (!available_[view->offset / kPage + page]) return false;
    std::vector<Change> changes;
    for (uint64_t page = first; page < end; ++page) append(changes, *view, page, access);
    return apply(changes);
}
bool SharedHeap::is_mapped(uint64_t address, uint64_t bytes) const {
    std::lock_guard lock(mutex_);
    auto* view = find_view(address, bytes);
    uint64_t first, end;
    if (!view || !span(address - view->base, bytes, view->size, first, end)) return false;
    for (uint64_t page = first; page < end; ++page)
        if (!available_[view->offset / kPage + page] || !(view->permissions[page] & 1)) return false;
    return true;
}
bool SharedHeap::is_committed(uint64_t address, uint64_t bytes) const {
    std::lock_guard lock(mutex_);
    auto* view = find_view(address, bytes);
    uint64_t first, end;
    if (!view || !span(address - view->base, bytes, view->size, first, end)) return false;
    first += view->offset / kPage; end += view->offset / kPage;
    return std::all_of(available_.begin() + first, available_.begin() + end,
                       [](uint8_t page) { return page != 0; });
}
uint64_t SharedHeap::view_end(uint64_t address) const {
    std::lock_guard lock(mutex_);
    const auto* view = find_view(address, 1);
    return view ? view->base + view->size : 0;
}

std::unique_ptr<ps5_vm::CopyView> SharedHeap::copy_view(uint64_t address, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    if (!primary_ || address < primary_->base) return {};
    const uint64_t offset = address - primary_->base;
    uint64_t first, end;
    if (!bytes || !span(offset, bytes, size_, first, end)) return {};
    for (uint64_t page = first; page < end; ++page) if (!active_[page]) return {};
    for (const auto* views : {&aliases_, &private_})
        for (const auto& [at, view] : *views) {
            (void)at;
            if (offset < view->offset + view->size && view->offset < offset + bytes) return {};
        }
    const uint64_t begin = offset & ~(host_page - 1), stop = up(offset + bytes, host_page);
    auto mapping = ps5_vm::Reservation::reserve(stop - begin);
    if (!mapping) return {};
    for (uint64_t at = begin; at < stop;) {
        const auto& block = blocks_[at / block_size];
        if (!block) return {};
        const uint64_t count = std::min(stop - at, block->size - at % block_size);
        if (!mapping.map(at - begin, block->memory, at % block_size, count, host_vm::Access::ReadWrite)) return {};
        at += count;
    }
    return std::make_unique<ps5_vm::CopyView>(ps5_vm::CopyView{std::move(mapping), offset - begin});
}

uint64_t SharedHeap::alias(uint64_t offset, uint64_t bytes, uint64_t at, Prot protection) {
    std::lock_guard lock(mutex_);
    const unsigned access = bits(protection);
    if (!primary_ || !bytes || bytes % kAllocGranularity || offset % kAllocGranularity ||
        offset > size_ || bytes > size_ - offset || at % kAllocGranularity || access > 3 ||
        at >= NX_GUEST_ADDRESS_END || bytes > NX_GUEST_ADDRESS_END - at) return 0;
    if (at) {
        auto view = make_view(offset, bytes, at, access, false);
        if (!view) return 0;
        aliases_.emplace(at, std::move(view)); return at;
    }
    uint64_t candidate = kAliasBase;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        for (const auto& [address, view] : aliases_)
            if (candidate < address + view->size && address < candidate + bytes) candidate = up(address + view->size, kAllocGranularity);
        if (candidate > kAliasBase + kAliasSize || bytes > kAliasBase + kAliasSize - candidate) return 0;
        auto view = make_view(offset, bytes, candidate, access, false);
        if (view) { aliases_.emplace(candidate, std::move(view)); return candidate; }
        candidate += bytes;
    }
    return 0;
}
bool SharedHeap::unmap_alias(uint64_t address) {
    std::lock_guard lock(mutex_);
    auto it = aliases_.find(address);
    if (it == aliases_.end() || !it->second->reset()) return false;
    aliases_.erase(it); return true;
}
void* SharedHeap::private_view(uint64_t offset, uint64_t bytes, bool require_backed) {
    std::lock_guard lock(mutex_);
    if (!primary_ || !bytes || offset % kAllocGranularity || offset > size_ || bytes > size_ - offset) return nullptr;
    // Vulkan may grant CPU/GPU access to the entire imported host range. Never
    // expose holes or partially committed host pages to that operation.
    if (require_backed) {
        if (bytes % host_page) return nullptr;
        for (uint64_t page = offset / kPage; page < (offset + bytes) / kPage; ++page)
            if (!available_[page]) return nullptr;
    }
    auto view = make_view(offset, bytes, 0, 3, true);
    if (!view) return nullptr;
    const uint64_t address = view->base;
    private_.emplace(address, std::move(view)); return reinterpret_cast<void*>(address);
}
bool SharedHeap::unmap_private(void* address) {
    std::lock_guard lock(mutex_);
    auto it = private_.find(reinterpret_cast<uint64_t>(address));
    if (it == private_.end() || !it->second->reset()) return false;
    private_.erase(it); return true;
}
bool SharedHeap::offset_of(uint64_t address, uint64_t* offset) const {
    if (!offset) return false;
    std::lock_guard lock(mutex_);
    auto* view = find_view(address, 1);
    if (!view) return false;
    *offset = view->offset + address - view->base; return true;
}
void SharedHeap::alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) const {
    std::lock_guard lock(mutex_);
    for (const auto& [address, view] : aliases_) { (void)address; out.emplace_back(view->offset, view->size); }
}
size_t SharedHeap::alias_count() const { std::lock_guard lock(mutex_); return aliases_.size(); }
uint64_t SharedHeap::reserved_bytes() const { std::lock_guard lock(mutex_); return primary_ ? size_ : 0; }
uint64_t SharedHeap::committed_bytes() const { std::lock_guard lock(mutex_); return committed_; }
uint64_t SharedHeap::backed_bytes() const {
    std::lock_guard lock(mutex_);
    uint64_t bytes = 0; for (const auto& block : blocks_) if (block) bytes += block->size;
    return bytes;
}
bool SharedHeap::reset() {
    std::lock_guard lock(mutex_);
    for (auto* views : {&private_, &aliases_})
        while (!views->empty()) {
            auto it = views->begin();
            if (!it->second->reset()) return false;
            views->erase(it);
        }
    if (primary_ && !primary_->reset()) return false;
    primary_.reset(); blocks_.clear(); available_.clear(); active_.clear(); committed_ = 0;
    return true;
}
} // namespace nxrt::mem
