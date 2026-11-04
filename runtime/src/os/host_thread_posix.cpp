// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/host_thread.h"
#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <utility>
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
    State(Entry function, void* arg) : entry(function), argument(arg) {}
};

Thread::Thread(std::unique_ptr<State> state) : state_(std::move(state)) {}
std::unique_ptr<Thread> Thread::create(Entry entry, void* argument, size_t stack_bytes) {
    if (!entry) { errno = EINVAL; return {}; }
    auto state = std::make_unique<State>(entry, argument);
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) { errno = error; return {}; }
    if (stack_bytes) error = pthread_attr_setstacksize(&attributes, stack_bytes);
    if (!error) error = pthread_attr_getstacksize(&attributes, &state->stack_size);
    // Allocate the owner before starting a worker: a failed C++ allocation must
    // never abandon a live thread that refers to freed state.
    std::unique_ptr<Thread> result;
    try { if (!error) result.reset(new Thread(std::move(state))); }
    catch (...) { pthread_attr_destroy(&attributes); throw; }
    if (!error) error = pthread_create(&result->state_->worker, &attributes, run, result->state_.get());
    pthread_attr_destroy(&attributes);
    if (error) {
        if (result) result->state_.reset(); // No worker owns this failed creation.
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
    state_->joined = true;
    return true;
}
bool Thread::is_current() const {
    return !state_->joined.load(std::memory_order_acquire) && pthread_equal(pthread_self(), state_->worker) != 0;
}
size_t Thread::stack_size() const { return state_->stack_size; }
std::exception_ptr Thread::failure() const {
    std::lock_guard lock(state_->mutex);
    return state_->failure;
}
[[noreturn]] void Thread::exit() { pthread_exit(nullptr); std::abort(); }
}
