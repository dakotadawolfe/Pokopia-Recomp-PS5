// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse fixed guest regions for module images, stacks and kernel allocations.
// Heap aliases have different decommit semantics and use a separate owner.
#pragma once
#include "nxrt/memory.h"
#include "nxrt/ps5_vm.h"
#include "nxrt/subpage.h"
#include <memory>
#include <mutex>
#include <vector>

namespace nxrt::mem {
class GuestRegion {
public:
    static std::unique_ptr<GuestRegion> create(uint64_t address, uint64_t size);
    ~GuestRegion();
    GuestRegion(const GuestRegion&) = delete;
    GuestRegion& operator=(const GuestRegion&) = delete;
    bool commit(uint64_t address, uint64_t size, Prot protection = Prot::RW);
    bool decommit(uint64_t address, uint64_t size);
    bool protect(uint64_t address, uint64_t size, Prot protection);
    bool is_mapped(uint64_t address, uint64_t size) const;
    uint64_t committed_bytes() const;
    uint64_t backed_bytes() const;
    // Stop guest accesses before release/destruction. Failed release keeps
    // ownership and permission metadata intact so the caller can retry.
    bool reset();
private:
    struct Block { ps5_vm::DirectMemory memory; uint64_t size; };
    GuestRegion(uint64_t address, uint64_t size, ps5_vm::Reservation reservation);
    bool span(uint64_t address, uint64_t size, uint64_t& first, uint64_t& end) const;
    void discard_new(const std::vector<size_t>& indices);
    mutable std::mutex mutex_;
    uint64_t base_, size_, committed_ = 0;
    ps5_vm::Reservation reservation_;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<uint8_t> committed_pages_;
    std::unique_ptr<SubpageMap> permissions_;
};
} // namespace nxrt::mem
