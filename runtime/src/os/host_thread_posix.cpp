// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/host_thread.h"
#include <algorithm>
#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <mutex>
#include <pthread.h>
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif
#include <thread>
#include <utility>
#ifdef NXRT_TARGET_PS5
#include "nxrt/ps5_vm.h"
#endif
#ifdef __GLIBC__
#include <bits/cxxabi_forced.h>
#endif

namespace nxrt::host_thread {
struct Thread::State {
    Entry entry;
    void* argument;
    size_t stack_size = 0;
    pthread_t worker{};
    std::mutex mutex, join_mutex;
    std::condition_variable changed;
    bool started = false, cancel_gate = false, finished = false;
    std::atomic<bool> joined{false};
    std::exception_ptr failure;
#ifdef NXRT_TARGET_PS5
    std::unique_ptr<ps5_vm::Reservation> stack;
#endif
    State(Entry function, void* arg) : entry(function), argument(arg) {}
};

Thread::Thread(std::unique_ptr<State> state) : state_(std::move(state)) {}
std::unique_ptr<Thread> Thread::create(Entry entry, void* argument, size_t stack_bytes) {
    if (!entry) { errno = EINVAL; return {}; }
    auto state = std::make_unique<State>(entry, argument);
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) { errno = error; return {}; }
    const char* stage = "attributes";
    if (stack_bytes) error = pthread_attr_setstacksize(&attributes, stack_bytes);
    if (!error) error = pthread_attr_getstacksize(&attributes, &state->stack_size);
    // Allocate the owner before starting a worker: a failed C++ allocation must
    // never abandon a live thread that refers to freed state.
    std::unique_ptr<Thread> result;
    try {
#ifdef NXRT_TARGET_PS5
        // Explicit large stack sizes bypass the graphics platform's default
        // stack wrapper and otherwise consume libkernel's flexible-memory pool.
        // Own direct backing until join, including pthread_exit and TLS cleanup.
        if (!error) {
            stage = "direct-stack";
            const size_t page = host_vm::page_size();
            if (!page || state->stack_size > std::numeric_limits<size_t>::max() - 2 * page) {
                error = EOVERFLOW;
            } else {
                const size_t bytes = ((state->stack_size + page - 1) / page) * page;
                auto mapping = ps5_vm::Reservation::reserve(bytes + page);
                if (!mapping) error = errno;
                else {
                    auto backing = ps5_vm::DirectMemory::allocate(bytes);
                    if (!backing || !mapping.map(page, backing, 0, bytes, host_vm::Access::ReadWrite)) error = errno;
                    else {
                        state->stack = std::make_unique<ps5_vm::Reservation>(std::move(mapping));
                        state->stack_size = bytes;
                        stage = "stack-attributes";
                        error = pthread_attr_setstack(&attributes,
                            static_cast<unsigned char*>(state->stack->data()) + page, bytes);
                        // The reserved, unbacked first page is our guard.
                        if (!error) error = pthread_attr_setguardsize(&attributes, 0);
                    }
                }
            }
        }
#endif
        if (!error) result.reset(new Thread(std::move(state)));
    }
    catch (...) { pthread_attr_destroy(&attributes); throw; }
    if (!error) {
        stage = "pthread-create";
        error = pthread_create(&result->state_->worker, &attributes, run, result->state_.get());
    }
    pthread_attr_destroy(&attributes);
    if (error) {
        if (result) result->state_.reset(); // No worker owns this failed creation.
        state.reset();
        std::fprintf(stderr, "HOST_THREAD_FAIL stage=%s requested_stack=%zu errno=%d\n", stage, stack_bytes, error);
        errno = error;
        return {};
    }
    return result;
}
Thread::~Thread() {
    if (state_ && !join()) std::abort();
}
void Thread::finish(void* raw) {
    auto& state = *static_cast<State*>(raw);
    std::lock_guard lock(state.mutex);
    state.finished = true;
    state.changed.notify_all();
}
void* Thread::run(void* raw) {
    auto& state = *static_cast<State*>(raw);
    // pthread_exit must signal completion as well as a normal return. The
    // native app's pthread wrapper drains C++ TLS before actual thread exit.
    pthread_cleanup_push(finish, raw);
    bool execute;
    {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.started || state.cancel_gate; });
        execute = !state.cancel_gate;
    }
    if (execute) {
        try { state.entry(state.argument); }
#ifdef __GLIBC__
        catch (const __cxxabiv1::__forced_unwind&) { throw; }
#endif
        catch (...) {
            std::lock_guard lock(state.mutex);
            state.failure = std::current_exception();
        }
    }
    pthread_cleanup_pop(1);
    return nullptr;
}
bool Thread::start() {
    std::lock_guard lock(state_->mutex);
    if (state_->started || state_->cancel_gate) { errno = EALREADY; return false; }
    state_->started = true;
    state_->changed.notify_all();
    return true;
}
bool Thread::wait_for(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(state_->mutex);
    if (timeout <= std::chrono::nanoseconds::zero()) return state_->finished;
    return state_->changed.wait_for(lock, timeout, [&] { return state_->finished; });
}
bool Thread::join() {
    // Serialize host joins; POSIX does not permit simultaneous pthread_join.
    if (state_->joined.load(std::memory_order_acquire)) return true;
    if (is_current()) { errno = EDEADLK; return false; }
    std::lock_guard join_lock(state_->join_mutex);
    if (state_->joined) return true;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->started) state_->cancel_gate = true;
        state_->changed.notify_all();
    }
    const int error = pthread_join(state_->worker, nullptr);
    if (error) { errno = error; return false; }
#ifdef NXRT_TARGET_PS5
    state_->stack.reset();
#endif
    state_->joined = true;
    return true;
}
bool Thread::is_current() const {
    return !state_->joined.load(std::memory_order_acquire) && pthread_equal(pthread_self(), state_->worker) != 0;
}
bool Thread::join_for(std::chrono::milliseconds timeout) {
    if (state_->joined.load(std::memory_order_acquire)) return true;
    if (is_current()) { errno = EDEADLK; return false; }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    const auto until = timeout >= maximum ? Clock::time_point::max() : now + std::max(timeout, std::chrono::milliseconds::zero());
    std::unique_lock join_lock(state_->join_mutex, std::defer_lock);
    while (!join_lock.try_lock()) {
        if (Clock::now() >= until) { errno = ETIMEDOUT; return false; }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (state_->joined) return true;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->started) state_->cancel_gate = true;
        state_->changed.notify_all();
    }
    // The FreeBSD/POSIX extension takes an absolute CLOCK_REALTIME deadline.
    // Use short bounded slices and the monotonic deadline for retries.
    for (;;) {
        auto left = until - Clock::now();
        auto span = std::min(std::max(std::chrono::duration_cast<std::chrono::nanoseconds>(left),
            std::chrono::nanoseconds::zero()), std::chrono::nanoseconds(100000000));
        timespec absolute{};
        if (clock_gettime(CLOCK_REALTIME, &absolute)) return false;
        absolute.tv_nsec += span.count();
        absolute.tv_sec += absolute.tv_nsec / 1000000000;
        absolute.tv_nsec %= 1000000000;
        const int error = pthread_timedjoin_np(state_->worker, nullptr, &absolute);
        if (!error) {
#ifdef NXRT_TARGET_PS5
            state_->stack.reset();
#endif
            state_->joined = true;
            return true;
        }
        if (error != ETIMEDOUT || Clock::now() >= until) { errno = error; return false; }
    }
}
size_t Thread::stack_size() const { return state_->stack_size; }
std::exception_ptr Thread::failure() const {
    std::lock_guard lock(state_->mutex);
    return state_->failure;
}
[[noreturn]] void Thread::exit() { pthread_exit(nullptr); std::abort(); }
}
