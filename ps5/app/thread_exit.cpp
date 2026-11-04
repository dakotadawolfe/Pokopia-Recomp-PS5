// SPDX-License-Identifier: GPL-3.0-or-later
// C++ thread-local destruction for the native app CRT. Guest threads and static
// C++ libraries share this registry; modules must remain loaded until exit.
#include <pthread.h>
#include <cstdlib>
#include <atomic>
#include <cerrno>

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

namespace {
struct Destructor { void (*fn)(void*); void* object; Destructor* next; };
pthread_once_t once = PTHREAD_ONCE_INIT;
pthread_key_t key;
int key_error = 0;
std::atomic<bool> initialized{false};

void drain(void* pending) {
    // POSIX clears the key before calling its destructor. Publish the remaining
    // list so destructors that register new objects keep reverse lifetime order.
    if (pthread_setspecific(key, pending) != 0) std::abort();
    while (auto* head = static_cast<Destructor*>(pthread_getspecific(key))) {
        if (pthread_setspecific(key, head->next) != 0) std::abort();
        const auto fn = head->fn; void* object = head->object;
        std::free(head);
        fn(object);
    }
}
void finish_current_thread() {
    if (initialized.load(std::memory_order_acquire)) drain(pthread_getspecific(key));
}
void initialize() {
    key_error = pthread_key_create(&key, drain);
    if (!key_error) {
        initialized.store(true, std::memory_order_release);
        if (std::atexit(finish_current_thread) != 0) std::abort();
    }
}
struct Start { void* (*fn)(void*); void* argument; };
void finish_worker(void*) { finish_current_thread(); }
void* run_worker(void* argument) {
    const Start start = *static_cast<Start*>(argument);
    std::free(argument);
    void* result;
    // The SDK frees emulated TLS in its pthread-key destructor. C++ object
    // destructors must run before that storage is freed, including pthread_exit
    // and cancellation. Cleanup handlers precede pthread-key destruction.
    pthread_cleanup_push(finish_worker, nullptr);
    result = start.fn(start.argument);
    pthread_cleanup_pop(1);
    return result;
}
}

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                      void* (*fn)(void*), void* argument) {
    auto* start = static_cast<Start*>(std::malloc(sizeof(Start)));
    if (!start) return ENOMEM;
    *start = {fn, argument};
    const int result = __real_pthread_create(thread, attributes, run_worker, start);
    if (result) std::free(start);
    return result;
}

extern "C" int __cxa_thread_atexit_impl(void (*fn)(void*), void* object, void* dso) {
    (void)dso;
    if (!fn || pthread_once(&once, initialize) != 0 || key_error) return -1;
    auto* entry = static_cast<Destructor*>(std::malloc(sizeof(Destructor)));
    if (!entry) return -1;
    *entry = {fn, object, static_cast<Destructor*>(pthread_getspecific(key))};
    if (pthread_setspecific(key, entry) != 0) { std::free(entry); return -1; }
    return 0;
}

// The application lifecycle calls this before asking the shell to close it.
// Payloads and host tests retain the atexit callback for process termination.
extern "C" void nxrt_ps5_finish_thread() { finish_current_thread(); }
