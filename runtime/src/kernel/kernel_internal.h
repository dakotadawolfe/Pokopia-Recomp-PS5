// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/kernel: state shared by thread.cpp, kernel_sync.cpp and svc_thread.cpp.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <algorithm>
#endif

#include "nxrt/kernel.h"

namespace nxrt::kernel::detail {

#ifdef _WIN32
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*), "KThread::cv_storage holds one CONDITION_VARIABLE");
inline CONDITION_VARIABLE* thread_cv(KThread& t) { return reinterpret_cast<CONDITION_VARIABLE*>(&t.cv_storage[0]); }
extern SRWLOCK g_k;
inline void lock_kernel() { AcquireSRWLockExclusive(&g_k); }
inline void unlock_kernel() { ReleaseSRWLockExclusive(&g_k); }
inline bool try_lock_kernel() { return TryAcquireSRWLockExclusive(&g_k) != 0; }
inline void notify_thread(KThread& t) { WakeConditionVariable(thread_cv(t)); }
inline void wait_thread(KThread& t, int64_t ns) {
    const uint64_t ms = ns < 0 ? INFINITE : static_cast<uint64_t>(ns / 1000000 + (ns % 1000000 != 0));
    SleepConditionVariableSRW(thread_cv(t), &g_k, ns < 0 ? INFINITE : static_cast<DWORD>(ms > 0x7FFFFFFE ? 0x7FFFFFFE : ms), 0);
}
#else
extern std::mutex g_k;
inline void lock_kernel() { g_k.lock(); }
inline void unlock_kernel() { g_k.unlock(); }
inline bool try_lock_kernel() { return g_k.try_lock(); }
inline void notify_thread(KThread& t) { t.wake_condition.notify_one(); }
inline void wait_thread(KThread& t, int64_t ns) {
    std::unique_lock lock(g_k, std::adopt_lock);
    // Bound each wait so a saturated guest deadline cannot overflow the host
    // library's time_point arithmetic. Callers recheck the absolute deadline.
    try {
        if (ns < 0) t.wake_condition.wait(lock);
        else t.wake_condition.wait_for(lock, std::chrono::nanoseconds(std::min<int64_t>(ns, 86400000000000LL)));
    } catch (...) { lock.release(); throw; }
    lock.release(); // The caller still owns the kernel lock.
}
#endif

struct KernelGuard {
    KernelGuard() { lock_kernel(); }
    ~KernelGuard() { unlock_kernel(); }
    KernelGuard(const KernelGuard&) = delete;
    KernelGuard& operator=(const KernelGuard&) = delete;
};

void sync_reset();                       // forget every wait queue (kernel_sync.cpp)
void thread_mark_exited(KThread& t);     // marks exited under the kernel lock and wakes WaitSynchronization waiters (kernel_sync.cpp)
// Blocks the calling thread on its own condition variable with g_k held until t.wake or the deadline (ns, < 0 = forever).
bool block_on_wake(KThread& t, int64_t timeout_ns);
void wake_thread(KThread& t, uint32_t result = 0);   // g_k held

}  // namespace nxrt::kernel::detail
