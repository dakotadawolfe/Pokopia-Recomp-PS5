// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/guest_region.h"
#include "nxrecomp/memory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace nxrt::mem {
namespace {
constexpr uint64_t host_page = 16384, block_size = 2 * 1024 * 1024;
unsigned access_bits(Prot protection) {
    switch (protection) {
        case Prot::None: return 0;
        case Prot::R: case Prot::RX: return 1;
        case Prot::RW: case Prot::RWX: return 3;
    }
    return 4;
}
uint64_t round_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
}

GuestRegion::GuestRegion(uint64_t address, uint64_t size, ps5_vm::Reservation reservation)
    : base_(address), size_(size), reservation_(std::move(reservation)),
      blocks_(round_up(size, block_size) / block_size), committed_pages_(size / kPage, 0) {}

std::unique_ptr<GuestRegion> GuestRegion::create(uint64_t address, uint64_t size) {
    if (!address || address % host_page || !size || address >= NX_GUEST_ADDRESS_END ||
        size > NX_GUEST_ADDRESS_END - address) return {};
    const uint64_t logical_size = round_up(size, kPage);
    const uint64_t reserved_size = round_up(size, host_page);
    auto reservation = ps5_vm::Reservation::reserve(reserved_size, address);
    if (!reservation) return {};
    auto region = std::unique_ptr<GuestRegion>(new GuestRegion(address, logical_size, std::move(reservation)));
    auto* owner = region.get();
    region->permissions_ = SubpageMap::create(address, reserved_size,
        [owner](uint64_t at, uint64_t length, host_vm::Access access) {
            return owner->reservation_.protect(at - owner->base_, length, access);
        });
    if (!region->permissions_) return {};
    return region;
}

GuestRegion::~GuestRegion() {
    if (!reset()) { std::perror("guest region: release"); std::abort(); }
}

bool GuestRegion::span(uint64_t address, uint64_t size, uint64_t& first, uint64_t& end) const {
    if (!permissions_ || address < base_ || address - base_ > size_ || size > size_ - (address - base_)) return false;
    first = (address - base_) / kPage;
    end = size ? round_up(address - base_ + size, kPage) / kPage : first;
    return true;
}

void GuestRegion::discard_new(const std::vector<size_t>& indices) {
    for (size_t index : indices) {
        // A rollback failure would keep an untracked mapping alive. Do not
        // release/reuse its physical memory and continue running guest code.
        if (!reservation_.discard(index * block_size, blocks_[index]->size)) {
            std::perror("guest region: backing rollback");
            std::abort();
        }
        blocks_[index].reset();
    }
}

bool GuestRegion::commit(uint64_t address, uint64_t size, Prot protection) {
    std::lock_guard lock(mutex_);
    uint64_t first, end;
    const unsigned bits = access_bits(protection);
    if (bits > 3 || !span(address, size, first, end)) return false;
    if (first == end) return true;
    const size_t begin_block = first * kPage / block_size;
    const size_t end_block = round_up(end * kPage, block_size) / block_size;
    std::vector<size_t> added;
    added.reserve(end_block - begin_block);
    // New physical pages are cleared through a private alias. The primary
    // guest range stays inaccessible until the entire commit is prepared.
    try {
        for (size_t index = begin_block; index < end_block; ++index) {
            if (blocks_[index]) continue;
            const uint64_t bytes = std::min(block_size, reservation_.size() - index * block_size);
            auto block = std::make_unique<Block>();
            block->size = bytes;
            block->memory = ps5_vm::DirectMemory::allocate(bytes);
            auto writable = ps5_vm::Reservation::reserve(bytes);
            if (!block->memory || !writable ||
                !writable.map(0, block->memory, 0, bytes, host_vm::Access::ReadWrite)) {
                discard_new(added); return false;
            }
            std::memset(writable.data(), 0, bytes);
            if (!reservation_.map(index * block_size, block->memory, 0, bytes, host_vm::Access::None)) {
                discard_new(added); return false;
            }
            blocks_[index] = std::move(block);
            added.push_back(index); // Capacity was reserved before the first mapping.
        }
        if (!permissions_->protect(base_ + first * kPage, (end - first) * kPage, bits)) {
            discard_new(added); return false;
        }
    } catch (...) {
        discard_new(added);
        throw;
    }
    for (uint64_t page = first; page < end; ++page) {
        if (!committed_pages_[page]) committed_ += kPage;
        committed_pages_[page] = 1;
    }
    return true;
}

bool GuestRegion::protect(uint64_t address, uint64_t size, Prot protection) {
    std::lock_guard lock(mutex_);
    uint64_t first, end;
    const unsigned bits = access_bits(protection);
    if (bits > 3 || !span(address, size, first, end)) return false;
    if (first == end) return true;
    for (uint64_t page = first; page < end; ++page) if (!committed_pages_[page]) return false;
    return permissions_->protect(base_ + first * kPage, (end - first) * kPage, bits);
}

bool GuestRegion::decommit(uint64_t address, uint64_t size) {
    std::lock_guard lock(mutex_);
    uint64_t first, end;
    if (!span(address, size, first, end)) return false;
    if (first == end) return true;
    struct ClearView { size_t index; ps5_vm::Reservation mapping; };
    std::vector<ClearView> clear_views;
    const size_t begin_block = first * kPage / block_size;
    const size_t end_block = round_up(end * kPage, block_size) / block_size;
    clear_views.reserve(end_block - begin_block);
    // Prepare every potentially failing alias before changing permissions.
    // Fixed-region decommit clears contents; heap decommit must not use this.
    for (size_t index = begin_block; index < end_block; ++index) {
        const auto& block = blocks_[index];
        if (!block) continue;
        auto writable = ps5_vm::Reservation::reserve(block->size);
        if (!writable || !writable.map(0, block->memory, 0, block->size, host_vm::Access::ReadWrite)) return false;
        clear_views.push_back({index, std::move(writable)});
    }
    if (!permissions_->protect(base_ + first * kPage, (end - first) * kPage, 0)) return false;
    for (const auto& view : clear_views) {
        const uint64_t begin = std::max(first * kPage, view.index * block_size);
        const uint64_t stop = std::min(end * kPage, view.index * block_size + blocks_[view.index]->size);
        std::memset(static_cast<unsigned char*>(view.mapping.data()) + begin - view.index * block_size, 0, stop - begin);
    }
    for (uint64_t page = first; page < end; ++page) {
        if (committed_pages_[page]) committed_ -= kPage;
        committed_pages_[page] = 0;
    }
    for (size_t index = begin_block; index < end_block; ++index) {
        const auto& block = blocks_[index];
        if (!block) continue;
        const uint64_t begin = index * block_size / kPage;
        const uint64_t stop = std::min<uint64_t>(committed_pages_.size(), begin + block->size / kPage);
        if (std::any_of(committed_pages_.begin() + begin, committed_pages_.begin() + stop,
                        [](uint8_t committed) { return committed != 0; })) continue;
        if (reservation_.discard(index * block_size, block->size)) blocks_[index].reset();
        else std::perror("guest region: retain inaccessible backing after failed reclaim");
    }
    return true;
}

bool GuestRegion::is_mapped(uint64_t address, uint64_t size) const {
    std::lock_guard lock(mutex_);
    uint64_t first, end;
    if (!span(address, size, first, end)) return false;
    if (first == end) return true;
    for (uint64_t page = first; page < end; ++page) if (!committed_pages_[page]) return false;
    return SubpageMap::allows(base_ + first * kPage, (end - first) * kPage, 1);
}
uint64_t GuestRegion::committed_bytes() const { std::lock_guard lock(mutex_); return committed_; }
uint64_t GuestRegion::backed_bytes() const {
    std::lock_guard lock(mutex_);
    uint64_t bytes = 0;
    for (const auto& block : blocks_) if (block) bytes += block->size;
    return bytes;
}
bool GuestRegion::reset() {
    std::lock_guard lock(mutex_);
    if (!reservation_.reset()) return false;
    permissions_.reset();
    blocks_.clear();
    committed_pages_.clear();
    committed_ = 0;
    return true;
}
} // namespace nxrt::mem
