// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/host_thread.h"
#include "nxrt/host_vm.h"
#include "nxrecomp/ctx.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <thread>
#ifdef NXRT_TARGET_PS5
#include <pthread.h>
#include <pthread_np.h>
extern "C" int sceKernelQueryMemoryProtection(void*, void**, void**, uint32_t*);
extern "C" int64_t sceKernelGetDirectMemorySize();
extern "C" int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
#endif

NXTEST_GROUP(host_thread)
using nxrt::host_thread::Thread;
using namespace std::chrono_literals;
namespace {
void increment(void* raw) { ++*static_cast<std::atomic<unsigned>*>(raw); }
std::atomic<unsigned> tls_destroyed{0};
struct Local {
    unsigned value = 0;
    ~Local() { tls_destroyed.fetch_add(value); }
};
thread_local Local local;
struct BlockingTls {
    std::atomic<bool>* entered = nullptr;
    std::atomic<bool>* release = nullptr;
    ~BlockingTls() {
        if (!entered) return;
        *entered = true;
        while (!release->load()) std::this_thread::sleep_for(1ms);
    }
};
thread_local BlockingTls blocking_tls;
extern "C" const NxModule nxmod_subpage_fixture;
}

NXTEST(host_worker_waits_for_start_and_cancels_unstarted_destruction) {
    std::atomic<unsigned> calls{0};
    {
        auto thread = Thread::create(increment, &calls);
        REQUIRE(thread);
        CHECK(!thread->wait_for(10ms));
        CHECK_EQ(calls.load(), 0u);
    }
    CHECK_EQ(calls.load(), 0u);
}
NXTEST(host_worker_starts_once_and_joins_repeatedly) {
    std::atomic<unsigned> calls{0};
    auto thread = Thread::create(increment, &calls);
    REQUIRE(thread);
    CHECK(thread->start());
    CHECK(!thread->start());
    CHECK_EQ(errno, EALREADY);
    CHECK(thread->join());
    CHECK(thread->join());
    CHECK(thread->wait_for(0ns));
    CHECK(!thread->failure());
    CHECK_EQ(calls.load(), 1u);
}
NXTEST(host_worker_join_cancels_an_unstarted_gate) {
    std::atomic<unsigned> calls{0};
    auto thread = Thread::create(increment, &calls);
    REQUIRE(thread);
    CHECK(thread->join());
    CHECK(thread->wait_for(0ns));
    CHECK(!thread->start());
    CHECK_EQ(calls.load(), 0u);
}
NXTEST(host_worker_timeout_does_not_end_work) {
    struct Context { std::atomic<unsigned> stage{0}; std::atomic<bool> release{false}; } context;
    auto thread = Thread::create(+[](void* raw) {
        auto& context = *static_cast<Context*>(raw);
        context.stage = 1;
        while (!context.release.load()) std::this_thread::sleep_for(1ms);
        context.stage = 2;
    }, &context);
    REQUIRE(thread);
    struct Release { Context& context; ~Release() { context.release = true; } } release{context};
    CHECK(thread->start());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!context.stage.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    CHECK_EQ(context.stage.load(), 1u);
    CHECK(!thread->wait_for(0ns));
    CHECK(!thread->wait_for(10ms));
    context.release = true;
    CHECK(thread->wait_for(5s));
    CHECK(thread->join());
    CHECK_EQ(context.stage.load(), 2u);
}
NXTEST(host_worker_retains_exception_for_its_owner) {
    auto thread = Thread::create(+[](void*) { throw std::runtime_error("worker failure"); }, nullptr);
    REQUIRE(thread);
    CHECK(thread->start());
    CHECK(thread->join());
    REQUIRE(thread->failure());
    bool caught = false;
    try { std::rethrow_exception(thread->failure()); }
    catch (const std::runtime_error& error) { caught = std::string(error.what()) == "worker failure"; }
    CHECK(caught);
}
NXTEST(host_worker_explicit_exit_runs_tls_cleanup) {
    tls_destroyed = 0;
    std::atomic<unsigned> stage{0};
    auto thread = Thread::create(+[](void* raw) {
        local.value = 17;
        *static_cast<std::atomic<unsigned>*>(raw) = 1;
        Thread::exit();
        *static_cast<std::atomic<unsigned>*>(raw) = 2;
    }, &stage);
    REQUIRE(thread);
    CHECK(thread->start());
    CHECK(thread->wait_for(5s));
    CHECK(thread->join());
    CHECK(!thread->failure());
    CHECK_EQ(stage.load(), 1u);
    CHECK_EQ(tls_destroyed.load(), 17u);
}
NXTEST(host_worker_self_join_is_rejected_without_deadlock) {
    struct Context { Thread* worker = nullptr; bool rejected = false; } context;
    auto thread = Thread::create(+[](void* raw) {
        auto& context = *static_cast<Context*>(raw);
        context.rejected = context.worker->is_current() && !context.worker->join() && errno == EDEADLK;
    }, &context);
    REQUIRE(thread);
    context.worker = thread.get();
    CHECK(!thread->is_current());
    CHECK(thread->start());
    CHECK(thread->join());
    CHECK(context.rejected);
}

NXTEST(host_worker_timed_join_includes_tls_destructors_and_can_retry) {
    std::array<std::atomic<bool>, 2> state{};
    auto thread = Thread::create(+[](void* raw) {
        auto& state = *static_cast<std::array<std::atomic<bool>, 2>*>(raw);
        blocking_tls.entered = &state[0]; blocking_tls.release = &state[1];
    }, &state);
    REQUIRE(thread);
    struct Release { std::atomic<bool>& flag; ~Release() { flag = true; } } release{state[1]};
    CHECK(thread->start());
    const auto limit = std::chrono::steady_clock::now() + 5s;
    while (!state[0] && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    CHECK(state[0]);
    CHECK(thread->wait_for(0ns)); // Entry cleanup is done; TLS destruction is blocked.
    const auto begin = std::chrono::steady_clock::now();
    CHECK(!thread->join_for(10ms));
    CHECK_EQ(errno, ETIMEDOUT);
    CHECK(std::chrono::steady_clock::now() - begin < 1s);
    state[1] = true;
    CHECK(thread->join_for(5s));
    CHECK(thread->join_for(0ms));
}
NXTEST(host_worker_validates_entry_and_stack) {
    CHECK(!Thread::create(nullptr, nullptr));
    CHECK_EQ(errno, EINVAL);
    std::atomic<unsigned> calls{0};
    CHECK(!Thread::create(increment, &calls, 1));
    CHECK_EQ(errno, EINVAL);
    auto thread = Thread::create(increment, &calls, 2u << 20);
    REQUIRE(thread);
    CHECK(thread->stack_size() >= 2u << 20);
    CHECK(thread->start());
    CHECK(thread->join());
    CHECK_EQ(calls.load(), 1u);
}
NXTEST(host_worker_repeated_creation_releases_tls_and_can_change_owner) {
    tls_destroyed = 0;
    for (unsigned i = 0; i < 32; ++i) {
        auto worker = Thread::create(+[](void*) { local.value = 1; }, nullptr);
        REQUIRE(worker);
        CHECK(worker->start());
        std::thread owner([worker = std::move(worker)]() mutable { worker.reset(); });
        owner.join();
    }
    CHECK_EQ(tls_destroyed.load(), 32u);
}
NXTEST(host_workers_execute_lifted_code_with_independent_registers) {
    REQUIRE(nxmod_subpage_fixture.nfns >= 2);
    REQUIRE(nxmod_subpage_fixture.fns[0].addr == 0x7100000000ull);
    REQUIRE(nxmod_subpage_fixture.fns[1].addr == 0x7100000040ull);
    namespace vm = nxrt::host_vm;
    const size_t size = vm::page_size();
    auto object = vm::SharedObject::create(size);
    REQUIRE(object);
    auto mapping = object.map(0, size, vm::Access::ReadWrite, 0x7100000000ull);
    REQUIRE(mapping);
    struct Context { Ctx registers{}; bool correct = false; };
    std::array<Context, 4> contexts{};
    std::array<std::unique_ptr<Thread>, 4> workers;
    for (size_t i = 0; i < workers.size(); ++i) {
        contexts[i].registers.x[0] = 0x1234 + i;
        contexts[i].registers.x[1] = reinterpret_cast<uint64_t>(mapping.data()) + i * 64;
        workers[i] = Thread::create(+[](void* raw) {
            auto& context = *static_cast<Context*>(raw);
            const auto value = context.registers.x[0];
            nxmod_subpage_fixture.fns[1].fn(&context.registers);
            context.registers.x[0] = 0;
            nxmod_subpage_fixture.fns[0].fn(&context.registers);
            context.correct = context.registers.x[0] == value;
        }, &contexts[i]);
        REQUIRE(workers[i]);
    }
    for (auto& worker : workers) CHECK(worker->start());
    for (size_t i = 0; i < workers.size(); ++i) {
        CHECK(workers[i]->join());
        CHECK(!workers[i]->failure());
        CHECK(contexts[i].correct);
        CHECK_EQ(*reinterpret_cast<uint64_t*>(contexts[i].registers.x[1]), uint64_t{0x1234} + i);
    }
}

#ifdef NXRT_TARGET_PS5
NXTEST(host_workers_use_concurrent_large_guarded_stacks_and_release_them_after_join) {
    constexpr size_t worker_count = 8;
    const auto page = nxrt::host_vm::page_size();
    int64_t free_at = -1;
    size_t free_bytes = 0;
    const int64_t limit = sceKernelGetDirectMemorySize();
    REQUIRE(limit >= 0);
    // The payload sandbox cannot allocate the game's native-app budget.
    // Exercise the same guard/ownership path at 8 MiB there, 64 MiB in an app.
    const size_t stack_bytes = (limit ? 64u : 8u) << 20;
    if (limit) {
        REQUIRE(sceKernelAvailableDirectMemorySize(0, limit, page, &free_at, &free_bytes) == 0);
        REQUIRE(free_bytes >= stack_bytes * worker_count + (256u << 20));
    }
    // Payloads report limit=0: allocation accepts that sentinel, but the free
    // range query does not. This is not a native-app capacity measurement.
    std::printf("HOST_STACK_BUDGET direct_limit=%lld largest_free=%zu query_available=%u concurrent_bytes=%zu\n",
        static_cast<long long>(limit), free_bytes, limit != 0, stack_bytes * worker_count);
    for (unsigned round = 0; round < 2; ++round) {
        struct Context {
            std::atomic<bool> ready{false}, release{false};
            uintptr_t base = 0;
            size_t size = 0;
            bool attributes_ok = false, inside_stack = false;
        };
        std::array<Context, worker_count> contexts{};
        std::array<std::unique_ptr<Thread>, worker_count> workers;
        struct Release {
            decltype(contexts)& items;
            ~Release() { for (auto& item : items) item.release = true; }
        } release{contexts};
        for (size_t i = 0; i < worker_count; ++i) {
            workers[i] = Thread::create(+[](void* raw) {
                auto& context = *static_cast<Context*>(raw);
                pthread_attr_t attributes;
                if (pthread_attr_init(&attributes) == 0) {
                    void* base = nullptr;
                    context.attributes_ok = pthread_attr_get_np(pthread_self(), &attributes) == 0 &&
                        pthread_attr_getstack(&attributes, &base, &context.size) == 0;
                    context.base = reinterpret_cast<uintptr_t>(base);
                    pthread_attr_destroy(&attributes);
                }
                volatile unsigned char local[1u << 20];
                for (size_t n = 0; n < sizeof local; n += 4096) local[n] = 0x5a;
                const auto address = reinterpret_cast<uintptr_t>(&local[0]);
                context.inside_stack = address >= context.base && address + sizeof local <= context.base + context.size;
                context.ready = true;
                while (!context.release) std::this_thread::sleep_for(1ms);
                for (size_t n = 0; n < sizeof local; n += 4096)
                    if (local[n] != 0x5a) context.inside_stack = false;
            }, &contexts[i], stack_bytes);
            REQUIRE(workers[i]);
            REQUIRE(workers[i]->start());
        }
        const auto until = std::chrono::steady_clock::now() + 5s;
        for (auto& context : contexts) {
            while (!context.ready && std::chrono::steady_clock::now() < until) std::this_thread::yield();
            REQUIRE(context.ready);
            CHECK(context.attributes_ok);
            CHECK_EQ(context.size, stack_bytes);
            CHECK(context.inside_stack);
            void* first = nullptr; void* last = nullptr;
            uint32_t flags = 0;
            REQUIRE(sceKernelQueryMemoryProtection(reinterpret_cast<void*>(context.base), &first, &last, &flags) == 0);
            CHECK_EQ(flags & 7u, 3u);
            flags = 0;
            const int guard = sceKernelQueryMemoryProtection(reinterpret_cast<void*>(context.base - page), &first, &last, &flags);
            CHECK(guard != 0 || (flags & 7u) == 0);
        }
        for (auto& context : contexts) context.release = true;
        for (size_t i = 0; i < worker_count; ++i) {
            // Both join paths must release our stack only after real TLS exit.
            REQUIRE(i % 2 ? workers[i]->join() : workers[i]->join_for(5s));
            CHECK(contexts[i].inside_stack);
            CHECK(!workers[i]->failure());
            void* first = nullptr; void* last = nullptr; uint32_t flags = 0;
            const int query = sceKernelQueryMemoryProtection(reinterpret_cast<void*>(contexts[i].base), &first, &last, &flags);
            CHECK(query != 0 || (flags & 7u) == 0);
        }
        std::printf("HOST_STACK_ROUND round=%u workers=%zu bytes_each=%zu\n", round, worker_count, stack_bytes);
    }
}
#endif
