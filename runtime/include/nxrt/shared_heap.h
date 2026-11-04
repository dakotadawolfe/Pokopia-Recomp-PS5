// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse shared backing with independent primary, guest-alias and import views.
#pragma once
#include "nxrt/memory.h"
#include "nxrt/ps5_vm.h"
#include "nxrt/subpage.h"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nxrt::mem {
class SharedHeap {
public:
    static std::unique_ptr<SharedHeap> create(uint64_t bytes);
    ~SharedHeap();
    SharedHeap(const SharedHeap&) = delete;
    SharedHeap& operator=(const SharedHeap&) = delete;
    bool commit(uint64_t offset, uint64_t bytes);
    // Section contents and aliases survive primary decommit/recommit.
    bool decommit(uint64_t offset, uint64_t bytes);
    bool commit_at(uint64_t address, uint64_t bytes, Prot protection);
    bool protect(uint64_t address, uint64_t bytes, Prot protection);
    bool is_mapped(uint64_t address, uint64_t bytes) const;
    bool is_committed(uint64_t address, uint64_t bytes) const;
    // Primary pages only; refuses overlapping aliases and GPU/import views.
    std::unique_ptr<ps5_vm::CopyView> copy_view(uint64_t address, uint64_t bytes);
    uint64_t view_end(uint64_t address) const;
    uint64_t alias(uint64_t offset, uint64_t bytes, uint64_t at = 0, Prot protection = Prot::RW);
    bool unmap_alias(uint64_t address);
    void* private_view(uint64_t offset, uint64_t bytes, bool require_backed = false);
    bool unmap_private(void* address);
    bool offset_of(uint64_t address, uint64_t* offset) const;
    void alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) const;
    size_t alias_count() const;
    uint64_t reserved_bytes() const;
    uint64_t committed_bytes() const;
    uint64_t backed_bytes() const;
    // All accesses must be quiescent before reset/destruction.
    bool reset();
private:
    struct View;
    struct Change;
    struct Binding;
    struct Block { ps5_vm::DirectMemory memory; uint64_t size; };
    explicit SharedHeap(uint64_t bytes);
    View* find_view(uint64_t address, uint64_t bytes) const;
    bool commit_view(View& target, uint64_t offset, uint64_t bytes, unsigned access, bool count);
    static void append(std::vector<Change>& changes, View& view, uint64_t page, unsigned access);
    static bool apply(std::vector<Change>& changes);
    bool attach(View& view, size_t block, const Block& backing, std::vector<Binding>* added = nullptr);
    void rollback_backing(const std::vector<Binding>& bindings, const std::vector<size_t>& blocks);
    std::unique_ptr<View> make_view(uint64_t offset, uint64_t bytes, uint64_t at, unsigned access, bool internal);
    mutable std::mutex mutex_;
    uint64_t size_, committed_ = 0;
    std::unique_ptr<View> primary_;
    std::map<uint64_t, std::unique_ptr<View>> aliases_, private_;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<uint8_t> available_, active_;
};
} // namespace nxrt::mem
