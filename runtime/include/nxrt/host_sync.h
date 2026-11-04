// SPDX-License-Identifier: GPL-3.0-or-later
// Host primitives used by the guest synchronization side table. Windows keeps
// SRW locks; PS5 and the host test build use the native C++ threading runtime.
#pragma once
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <time.h>
#ifdef __FreeBSD__
#include <pthread.h>
#include <pthread_np.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace nxrt::host {
using ThreadId = uint32_t;
constexpr uint32_t infinite = 0xffffffffu;
#ifdef _WIN32
struct Mutex { SRWLOCK value = SRWLOCK_INIT; };
struct RwLock { SRWLOCK value = SRWLOCK_INIT; };
struct Condition { CONDITION_VARIABLE value = CONDITION_VARIABLE_INIT; };
template <class Lock> inline void lock_exclusive(Lock* l) { AcquireSRWLockExclusive(&l->value); }
template <class Lock> inline void unlock_exclusive(Lock* l) { ReleaseSRWLockExclusive(&l->value); }
inline void lock_shared(RwLock* l) { AcquireSRWLockShared(&l->value); }
inline void unlock_shared(RwLock* l) { ReleaseSRWLockShared(&l->value); }
inline void wake_one(Condition* c) { WakeConditionVariable(&c->value); }
inline void wake_all(Condition* c) { WakeAllConditionVariable(&c->value); }
inline void wait(Condition* c, Mutex* m, uint32_t ms) { SleepConditionVariableSRW(&c->value, &m->value, ms, 0); }
inline ThreadId thread_id() { return GetCurrentThreadId(); }
inline int64_t monotonic_ns() {
    static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart / frequency) * 1000000000LL +
        static_cast<int64_t>(static_cast<double>(now.QuadPart % frequency) * 1e9 / frequency);
}
#else
using Mutex = std::mutex;
using RwLock = std::shared_mutex;
using Condition = std::condition_variable;
template <class Lock> inline void lock_exclusive(Lock* l) { l->lock(); }
template <class Lock> inline void unlock_exclusive(Lock* l) { l->unlock(); }
inline void lock_shared(RwLock* l) { l->lock_shared(); }
inline void unlock_shared(RwLock* l) { l->unlock_shared(); }
inline void wake_one(Condition* c) { c->notify_one(); }
inline void wake_all(Condition* c) { c->notify_all(); }
inline void wait(Condition* c, Mutex* m, uint32_t ms) {
    std::unique_lock<Mutex> lock(*m, std::adopt_lock);
    if (ms == infinite) c->wait(lock);
    else c->wait_for(lock, std::chrono::milliseconds(ms));
    lock.release(); // The caller still owns the lock after the wait.
}
inline ThreadId thread_id() {
#ifdef __FreeBSD__
    static thread_local const auto id = static_cast<ThreadId>(pthread_getthreadid_np());
#else
    static thread_local const auto id = static_cast<ThreadId>(syscall(SYS_gettid));
#endif
    return id;
}
inline int64_t monotonic_ns() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) std::abort();
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}
#endif
} // namespace nxrt::host
