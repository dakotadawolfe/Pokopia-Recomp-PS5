// SPDX-License-Identifier: GPL-3.0-or-later
// Horizon thread objects backed by owned POSIX workers. Guest priorities order
// kernel wait queues; host affinity and priority policy are a separate porting
// step. Workers currently use the process's inherited scheduling policy.
#include "kernel_internal.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#ifdef __GLIBC__
#include <bits/cxxabi_forced.h>
#endif

namespace nxrt::kernel {
namespace {
std::mutex list_mutex;
std::vector<std::shared_ptr<KThread>> all_threads;
std::atomic<uint64_t> next_id{0x50};
thread_local KThread* self = nullptr;

struct Gauge {
    Gauge() { gauge_register("kernel.threads", [] {
        std::lock_guard lock(list_mutex);
        return static_cast<int64_t>(all_threads.size());
    }); }
    ~Gauge() { gauge_unregister("kernel.threads"); }
} thread_gauge;

void finish(void* raw) {
    self = nullptr;
    detail::thread_mark_exited(*static_cast<KThread*>(raw));
}
void run(void* raw) {
    auto* t = static_cast<KThread*>(raw);
    self = t;
    log_set_thread_name(t->name.c_str());
    std::exception_ptr failure;
    pthread_cleanup_push(finish, t);
    try { dispatch_call(&t->ctx, t->entry); }
#ifdef __GLIBC__
    catch (const __cxxabiv1::__forced_unwind&) { throw; }
#endif
    catch (...) { failure = std::current_exception(); }
    // FreeBSD's cleanup stack does not unwind with a C++ exception. Pop it
    // before rethrowing to the worker owner, which retains the exception.
    pthread_cleanup_pop(1);
    if (failure) std::rethrow_exception(failure);
}

void remember(const std::shared_ptr<KThread>& t) {
    std::vector<std::shared_ptr<KThread>> retired;
    {
        std::lock_guard lock(list_mutex);
        for (auto it = all_threads.begin(); it != all_threads.end();) {
            if ((*it)->exited && it->use_count() == 1) {
                retired.push_back(std::move(*it));
                it = all_threads.erase(it);
            } else ++it;
        }
        all_threads.push_back(t);
    }
    // Joining may run TLS destructors that inspect the thread list.
    retired.clear();
}
}

KThread::~KThread() = default; // The worker owner joins before its state is freed.
KThread* current_thread() { return self; }
::Ctx* current_ctx() { return self ? &self->ctx : nullptr; }
int app_core_count() { return game::info().platform == game::Platform::Switch2 ? 6 : 3; }

uint32_t thread_create(const ThreadParams& p) {
    if (p.prio < 0 || p.prio > 63 || p.core < -2 || p.core > 63) return 0;
    auto t = std::make_shared<KThread>();
    t->self = t;
    t->id = next_id++;
    t->name = p.name.empty() ? "guest-" + std::to_string(t->id) : p.name;
    t->entry = p.entry;
    t->arg = p.arg;
    t->stack_top = p.stack_top;
    t->prio = p.prio;
    const int core = p.core == -2 ? 0 : p.core;
    t->ideal_core = std::min(core, app_core_count() - 1);
    t->core_mask = 1ull << std::max(0, t->ideal_core);
    t->host_stack_bytes = p.host_stack_bytes;

    const TlsInfo& tpl = module_tls_template();
    constexpr uint64_t limit = mem::kKernelSize;
    if (tpl.tp_offset > limit || tpl.mem_size > limit - tpl.tp_offset ||
        tpl.file_size > tpl.mem_size || tpl.tp_offset + tpl.mem_size > limit - 0x10ff) return 0;
    const uint64_t block_size = std::max<uint64_t>(0x1000, (tpl.tp_offset + tpl.mem_size + 0x10ff) & ~0xfffull);
    t->tls = mem::alloc_kernel(0x1000);
    t->tp_block = mem::alloc_kernel(block_size);
    if (!t->tls || !t->tp_block) return 0;
    if (tpl.file_size) std::memcpy(reinterpret_cast<void*>(t->tp_block + tpl.tp_offset),
        reinterpret_cast<const void*>(tpl.init_addr), static_cast<size_t>(tpl.file_size));
    *reinterpret_cast<uint64_t*>(t->tls + 0x1f0) = t->tp_block;
    t->ctx.ex_addr = ~0ull;
    t->ctx.x[0] = p.arg;
    t->ctx.x[31] = p.stack_top;
    t->ctx.tpidr = t->tp_block;
    t->ctx.tpidrro = t->tls;
    t->handle = handle_add(t);
    if (!t->handle) return 0;
    try { remember(t); }
    catch (...) { handle_close(t->handle); throw; }
    return t->handle;
}

uint32_t thread_start(uint32_t handle) {
    auto t = handle_as<KThread>(handle);
    if (!t) return kInvalidHandle;
    {
        detail::KernelGuard lock;
        if (t->started) return kInvalidState;
    }
    std::lock_guard host_lock(t->host_mutex);
    {
        detail::KernelGuard lock;
        if (t->started) return kInvalidState;
    }
    uint64_t bytes = t->host_stack_bytes;
    if (!bytes) {
        // Retain the lifted runtime's deep-call-stack allowance. Stack usage
        // and a smaller PS5 default need measurements with actual game code.
        const auto mb = config_get_int("threads", "host_stack_mb", 64);
        if (mb < 1 || mb > 1024) return kOutOfResource;
        bytes = static_cast<uint64_t>(mb) << 20;
    }
    if (bytes > std::numeric_limits<size_t>::max()) return kOutOfResource;
    try { t->worker = host_thread::Thread::create(run, t.get(), static_cast<size_t>(bytes),
        config_get_bool("threads", "measure_host_stack", false)); }
    catch (const std::bad_alloc&) { return kOutOfMemory; }
    if (!t->worker) {
        NXLOG_E("kernel", "host thread creation failed: handle=%x entry=%llx stack=%llu errno=%d",
                handle, static_cast<unsigned long long>(t->entry), static_cast<unsigned long long>(bytes), errno);
        return kOutOfResource;
    }
    {
        detail::KernelGuard lock;
        t->started = true;
    }
    if (!t->worker->start()) {
        NXLOG_E("kernel", "host thread start gate failed: handle=%x errno=%d", handle, errno);
        t->worker.reset();
        detail::KernelGuard lock;
        t->started = false;
        return kOutOfResource;
    }
    NXLOG_I("kernel", "host thread started: handle=%x entry=%llx stack=%zu", handle,
            static_cast<unsigned long long>(t->entry), t->worker->stack_size());
    return kOk;
}

[[noreturn]] void thread_exit() {
    if (self && !self->worker) main_thread_release();
    host_thread::Thread::exit();
}
uint32_t main_thread_adopt(uint64_t stack_size, const char* name) {
    if (self || !stack_size || stack_size > mem::kKernelSize || stack_size % mem::kPage) return 0;
    const uint64_t stack = mem::alloc_kernel(stack_size, mem::kAllocGranularity);
    if (!stack) return 0;
    ThreadParams p;
    p.stack_top = stack + stack_size;
    p.name = name ? name : "main";
    const uint32_t handle = thread_create(p);
    if (!handle) return 0;
    auto t = handle_as<KThread>(handle);
    mem::vm_set(stack, stack_size, mem::kMemStack, 3);
    {
        detail::KernelGuard lock;
        t->started = true;
    }
    self = t.get();
    log_set_thread_name(t->name.c_str());
    return handle;
}
void main_thread_release() {
    if (self && !self->worker) finish(self);
}
std::vector<std::shared_ptr<KThread>> threads() {
    std::lock_guard lock(list_mutex);
    return all_threads;
}
bool thread_join(uint32_t handle, uint32_t timeout_ms) {
    auto t = handle_as<KThread>(handle);
    if (!t) return true;
    if (t.get() == self) return false;
    host_thread::Thread* worker;
    {
        std::lock_guard lock(t->host_mutex);
        worker = t->worker.get();
        if (!worker) {
            detail::KernelGuard kernel_lock;
            return !t->started || t->exited;
        }
    }
    // A started worker is immutable and t retains its owner while joining.
    return timeout_ms == UINT32_MAX ? worker->join() : worker->join_for(std::chrono::milliseconds(timeout_ms));
}
uint64_t thread_count_running() {
    auto all = threads();
    detail::KernelGuard lock;
    return std::count_if(all.begin(), all.end(), [](const auto& t) { return t->started && !t->exited; });
}
bool thread_set_priority(KThread& t, int priority) {
    if (priority < 0 || priority > 63) return false;
    detail::KernelGuard lock;
    t.prio = priority;
    return true;
}
bool thread_set_core_mask(KThread& t, int ideal, uint64_t mask) {
    const int n = app_core_count();
    uint64_t usable = mask & ((1ull << n) - 1);
    if (n == 3 && (mask & 8)) usable |= 4;
    if (!usable || ideal < -3) return false;
    detail::KernelGuard lock;
    if (ideal == -3) ideal = t.ideal_core;
    if (ideal == -2) ideal = 0;
    ideal = std::min(ideal, n - 1);
    if (ideal < 0 || !(usable >> ideal & 1)) {
        for (ideal = 0; !(usable >> ideal & 1); ++ideal) {}
    }
    t.ideal_core = ideal;
    t.core_mask = usable;
    return true;
}
void kernel_reset() {
    if (self) throw std::logic_error("release the current guest thread before kernel_reset");
    auto all = threads();
    for (auto& t : all) {
        std::lock_guard lock(t->host_mutex);
        if (t->worker) {
            if (!t->worker->join_for(std::chrono::seconds(5)))
                throw std::runtime_error("kernel_reset refused: a guest worker is still running");
        } else if (t->started && !t->exited) {
            throw std::runtime_error("kernel_reset refused: an adopted guest thread is still running");
        }
    }
    {
        std::lock_guard lock(list_mutex);
        all_threads.clear(); // The local snapshot retains owners until outside the lock.
    }
    handles_reset();
    detail::sync_reset();
}
}
