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
