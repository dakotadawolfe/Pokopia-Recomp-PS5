// SPDX-License-Identifier: GPL-3.0-or-later
// Guest permissions for a backed region on a host with 16 KiB pages. Callers
// own the mappings and initialize backing before publishing access here.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "nxrt/host_vm.h"

namespace nxrt::mem {
class SubpageMap {
public:
    // Callback protects a run of whole host pages at absolute guest addresses.
    // A failed call must leave that run unchanged. Destroy only after guest
    // accesses are quiescent; the caller releases the backing afterward.
    using Apply = std::function<bool(uint64_t, uint64_t, host_vm::Access)>;
    static std::unique_ptr<SubpageMap> create(uint64_t base, uint64_t size, Apply apply);
    ~SubpageMap();
    SubpageMap(const SubpageMap&) = delete;
    SubpageMap& operator=(const SubpageMap&) = delete;
    // Guest-aligned range; access uses Horizon bits R=1, W=2. Execute is handled
    // by the function dispatcher; original ARM instructions are never executed.
    bool protect(uint64_t address, uint64_t size, unsigned access);
    static bool allows(uint64_t address, uint64_t size, unsigned access);
private:
    SubpageMap(uint64_t base, uint64_t size, Apply apply);
    uint64_t base_, size_;
    Apply apply_;
    bool registered_ = false;
    std::vector<uint8_t> permissions_;
};
} // namespace nxrt::mem
