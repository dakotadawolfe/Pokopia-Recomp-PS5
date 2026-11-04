// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel handle table (include/nxrt/kernel.h). Pattern from SuperMarioWonderRecomp runtime/src/kernel.cpp handle_add/get/close (MIT).
#include <shared_mutex>
#include <mutex>

#include <functional>
#include <unordered_map>

#include "nxrt/gauge.h"
#include "nxrt/kernel.h"

namespace nxrt::kernel {
namespace {
std::shared_mutex g_lock;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next = 0x10000;
}  // namespace

uint32_t handle_add(std::shared_ptr<KObject> o) {
    if (!o) return 0;
    std::unique_lock lock(g_lock);
    uint32_t h = 0;
    if (g_handles.size() < 0x3000) {          // Horizon's per-process handle table holds 0x400..0x3000 entries
        do {
            h = g_next++;
            if (g_next >= 0x40000000) g_next = 0x10000;
        } while (g_handles.contains(h));
        g_handles[h] = std::move(o);
    }
    lock.unlock();
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    if (h == kPseudoCurrentThread) {
        KThread* t = current_thread();
        return t ? t->self.lock() : nullptr;
    }
    std::shared_lock lock(g_lock);
    auto it = g_handles.find(h);
    std::shared_ptr<KObject> r = it == g_handles.end() ? nullptr : it->second;
    lock.unlock();
    return r;
}

void handle_close(uint32_t h) {
    std::shared_ptr<KObject> dying;   // destroyed outside the lock
    std::unique_lock lock(g_lock);
    auto it = g_handles.find(h);
    if (it != g_handles.end()) {
        dying = std::move(it->second);
        g_handles.erase(it);
    }
    lock.unlock();
}

size_t handle_count() {
    std::shared_lock lock(g_lock);
    size_t n = g_handles.size();
    lock.unlock();
    return n;
}

void handles_reset() {
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> old;
    std::unique_lock lock(g_lock);
    old.swap(g_handles);
    g_next = 0x10000;
    lock.unlock();
}

// ---- gauges (include/nxrt/gauge.h): live handles per object type ----
namespace {
// Removes its gauge when the file's statics are torn down (the hooks are declared after g_handles, so they go first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};

// Handle-table entries of one object type: a walk of at most 0x3000 entries under the table lock (shared).
size_t count_type(ObjType t) {
    std::shared_lock lock(g_lock);
    size_t n = 0;
    for (const auto& kv : g_handles)
        if (kv.second->type() == t) n++;
    lock.unlock();
    return n;
}

GaugeHook gauge_handles("kernel.handles", [] { return static_cast<int64_t>(handle_count()); });
GaugeHook gauge_obj_thread("kernel.obj_thread", [] { return static_cast<int64_t>(count_type(ObjType::Thread)); });
GaugeHook gauge_obj_event("kernel.obj_event", [] { return static_cast<int64_t>(count_type(ObjType::Event)); });
GaugeHook gauge_obj_session("kernel.obj_session", [] { return static_cast<int64_t>(count_type(ObjType::Session)); });
GaugeHook gauge_obj_shared_mem("kernel.obj_shared_mem", [] { return static_cast<int64_t>(count_type(ObjType::SharedMemory)); });
GaugeHook gauge_obj_transfer_mem("kernel.obj_transfer_mem", [] { return static_cast<int64_t>(count_type(ObjType::TransferMemory)); });
GaugeHook gauge_obj_process("kernel.obj_process", [] { return static_cast<int64_t>(count_type(ObjType::Process)); });
GaugeHook gauge_obj_port("kernel.obj_port", [] { return static_cast<int64_t>(count_type(ObjType::Port)); });
}  // namespace

}  // namespace nxrt::kernel
