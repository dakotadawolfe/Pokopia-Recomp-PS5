// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/kernel.h"
#include "nxrt/host_vm.h"
#include "nxrt/memory.h"
#include "nxrt/subpage.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace nxrt::kernel {
struct SharedMemoryBacking {
    host_vm::SharedObject object;
    host_vm::Mapping service;
    SharedMemoryBacking(host_vm::SharedObject obj, host_vm::Mapping view)
        : object(std::move(obj)), service(std::move(view)) {}
};
namespace {
constexpr uint64_t host_page = 0x4000;
uint64_t round_host(uint64_t size) { return (size + host_page - 1) & ~(host_page - 1); }
struct View {
    std::shared_ptr<KSharedMemory> owner;
    host_vm::Mapping mapping;
    std::unique_ptr<mem::SubpageMap> policy;
    View(std::shared_ptr<KSharedMemory> obj, host_vm::Mapping view)
        : owner(std::move(obj)), mapping(std::move(view)) {}
    bool release() {
        if (!mapping.reset()) return false;
        policy.reset(); return true;
    }
    ~View() {
        if (!release()) { std::perror("shared memory: release view"); std::abort(); }
    }
};
std::mutex mutex;
std::map<uint64_t, std::unique_ptr<View>> views;
uint32_t validate(uint64_t address, uint64_t size) {
    if (address % mem::kPage) return kInvalidAddress;
    if (!size || size % mem::kPage) return kInvalidSize;
    if (size > UINT64_MAX - address) return kInvalidMemoryState;
    return kOk;
}
bool map_region(uint64_t address, uint64_t size) {
    // Same guest map region as the existing memory SVC backend. The exact
    // native mapping is non-destructive even when another subsystem owns it.
    constexpr uint64_t base = 0x8000000;
    return address >= base && address < mem::kMainBase && size <= mem::kMainBase - address;
}
}
std::shared_ptr<KSharedMemory> shared_memory_create(uint64_t size, const char* name, uint32_t permission) {
    if (!size || size % mem::kPage || size > mem::kKernelSize || (permission != 1 && permission != 3)) return {};
    auto object = host_vm::SharedObject::create(round_host(size));
    if (!object) return {};
    auto service = object.map(0, object.size(), host_vm::Access::ReadWrite);
    if (!service) return {};
    auto result = std::make_shared<KSharedMemory>();
    result->backing = std::make_shared<SharedMemoryBacking>(std::move(object), std::move(service));
    result->host = reinterpret_cast<uint64_t>(result->backing->service.data());
    result->size = size; result->name = name ? name : ""; result->guest_permission = permission;
    return result;
}
uint32_t shared_memory_map(uint32_t handle, uint64_t address, uint64_t size, uint32_t permission) {
    if (auto rc = validate(address, size)) return rc;
    if (permission != 1 && permission != 3) return kInvalidPermission;
    auto object = handle_as<KSharedMemory>(handle);
    if (!object) return kInvalidHandle;
    if (!map_region(address, size)) return kInvalidMemoryRegion;
    if (size != object->size) return kInvalidSize;
    if (!object->backing) return kNotImplemented;
    if (permission != object->guest_permission) return kInvalidPermission;
    // PS5 cannot map one direct-memory page at a 4 KiB offset. Refuse the
    // occupied candidate, allowing the SDK's random-address mapper to retry.
    if (address % host_page) return kInvalidMemoryState;
    const auto bytes = round_host(size);
    if (!map_region(address, bytes)) return kInvalidMemoryRegion;
    std::lock_guard lock(mutex);
    auto info = mem::vm_query(address);
    if (info.state != mem::kMemFree || address - info.addr > info.size || bytes > info.size - (address - info.addr))
        return kInvalidMemoryState;
    auto mapping = object->backing->object.map(0, bytes, host_vm::Access::None, address);
    if (!mapping) return kInvalidMemoryState;
    auto view = std::make_unique<View>(object, std::move(mapping));
    auto* owner = view.get();
    view->policy = mem::SubpageMap::create(address, bytes,
        [owner, address](uint64_t at, uint64_t count, host_vm::Access access) {
            return owner->mapping.protect(at - address, count, access);
        });
    if (!view->policy || !view->policy->protect(address, size, permission)) return kOutOfMemory;
    views.emplace(address, std::move(view));
    mem::vm_set(address, size, mem::kMemShared, permission);
    // Padding is not guest memory. Report the logical end as free so the SDK
    // can verify its surrounding guard space. The host reservation still
    // prevents another view from replacing this physical page, and SubpageMap
    // refuses guest reads/writes beyond the object's exact logical size.
    if (size < bytes) mem::vm_set(address + size, bytes - size, mem::kMemFree, 0);
    return kOk;
}
uint32_t shared_memory_unmap(uint32_t handle, uint64_t address, uint64_t size) {
    if (auto rc = validate(address, size)) return rc;
    auto object = handle_as<KSharedMemory>(handle);
    if (!object) return kInvalidHandle;
    if (!map_region(address, size)) return kInvalidMemoryRegion;
    if (size != object->size) return kInvalidSize;
    std::lock_guard lock(mutex);
    auto it = views.find(address);
    if (it == views.end() || it->second->owner != object) return kInvalidMemoryState;
    if (!it->second->release()) return kInvalidMemoryState;
    mem::vm_set(address, round_host(size), mem::kMemFree, 0);
    views.erase(it);
    return kOk;
}
void shared_memory_reset_for_tests() {
    std::lock_guard lock(mutex);
    for (auto& [address, view] : views) {
        const auto size = round_host(view->owner->size);
        if (!view->release()) { std::perror("shared memory: reset view"); std::abort(); }
        mem::vm_set(address, size, mem::kMemFree, 0);
    }
    views.clear();
}
} // namespace nxrt::kernel
