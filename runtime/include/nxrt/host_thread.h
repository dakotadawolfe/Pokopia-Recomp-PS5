// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>

namespace nxrt::host_thread {
// Owns one POSIX worker. Creation leaves it behind a start gate, so a guest
// thread's registers, handle and scheduling can be published before execution.
// Destruction joins a running worker, or cancels an unstarted gate. The owner
// must outlive its entry function; destroying it on its own worker is invalid.
class Thread {
public:
    using Entry = void (*)(void*);
    static std::unique_ptr<Thread> create(Entry entry, void* argument, size_t stack_bytes = 0);
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    bool start();
    // Waits for the entry/cleanup handler. TLS destructors may still be running;
    // join(), not this observation, establishes full host-thread termination.
    bool wait_for(std::chrono::nanoseconds timeout);
    bool join(); // Also cancels an unstarted gate. False for a self-join.
    bool is_current() const;
    size_t stack_size() const;
    std::exception_ptr failure() const; // Inspect after wait_for or join.
    [[noreturn]] static void exit();
private:
    struct State;
    explicit Thread(std::unique_ptr<State> state);
    static void* run(void* state);
    static void finish(void* state);
    std::unique_ptr<State> state_;
};
}
