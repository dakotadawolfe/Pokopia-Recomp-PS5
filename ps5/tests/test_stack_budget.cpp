// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/host_thread.h"
#include "nxrt/host_vm.h"
#include <array>
#include <atomic>
#include <pthread.h>
#include <pthread_np.h>
#include <thread>

extern "C" int sceKernelQueryMemoryProtection(void*, void**, void**, uint32_t*);
NXTEST_GROUP(stack_budget)
namespace {
// A returned deep frame distinguishes a painted watermark from a sample of
// the current stack pointer. Touch every page, including both ends.
template<size_t FrameBytes> __attribute__((noinline)) unsigned deep_stack_frame() {
    volatile unsigned char data[FrameBytes];
    unsigned total = 0;
    for (size_t n = 0; n < sizeof data; n += 4096) data[n] = 0x5a;
    data[sizeof data-1] = 0x73;
    for (size_t n = 0; n < sizeof data; n += 4096) total += data[n];
    return total + data[sizeof data-1];
}
}
template<size_t StackBytes, size_t FrameBytes> void check_worker_stacks() {
    using nxrt::host_thread::Thread;
    using namespace std::chrono_literals;
    constexpr size_t bytes = StackBytes;
    struct Context {
        std::atomic<bool> ready{false}, release{false};
        nxrt::host_thread::CurrentSample sample;
        uintptr_t base = 0;
        size_t capacity = 0;
        unsigned checksum = 0;
        bool attributes_ok = false;
    };
    std::array<Context, 2> contexts;
    std::array<std::unique_ptr<Thread>, 2> workers;
    struct Release {
        decltype(contexts)& contexts;
        ~Release() { for (auto& context : contexts) context.release = true; }
    } release{contexts};
    for (size_t n = 0; n < workers.size(); ++n) {
        workers[n] = Thread::create(+[](void* raw) {
            auto& context = *static_cast<Context*>(raw);
            context.checksum = deep_stack_frame<FrameBytes>();
            context.sample = Thread::sample_current();
            pthread_attr_t attributes;
            if (!pthread_attr_init(&attributes)) {
                void* base = nullptr;
                context.attributes_ok = !pthread_attr_get_np(pthread_self(), &attributes) &&
                    !pthread_attr_getstack(&attributes, &base, &context.capacity);
                context.base = reinterpret_cast<uintptr_t>(base);
                pthread_attr_destroy(&attributes);
            }
            context.ready = true;
            while (!context.release) std::this_thread::sleep_for(1ms);
        }, &contexts[n], bytes, true);
        REQUIRE(workers[n]); REQUIRE(workers[n]->start());
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (auto& context : contexts) {
        while (!context.ready && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE(context.ready); REQUIRE(context.attributes_ok);
        CHECK_EQ(context.checksum, unsigned(FrameBytes/4096*0x5a + 0x73));
        CHECK_EQ(context.capacity, bytes); CHECK_EQ(context.sample.stack_bytes, bytes);
        CHECK(context.sample.stack_used < 65536);
        CHECK(context.sample.touched_stack_bytes >= FrameBytes);
        CHECK(context.sample.touched_stack_bytes < bytes - (1u << 20));
        void* first = nullptr; void* last = nullptr; uint32_t flags = 0;
        const int guard = sceKernelQueryMemoryProtection(reinterpret_cast<void*>(context.base-nxrt::host_vm::page_size()), &first, &last, &flags);
        CHECK(guard != 0 || (flags & 7u) == 0);
        std::printf("STACK_WATERMARK capacity=%zu point=%zu touched=%zu guard_rc=%08x\n",
            bytes, context.sample.stack_used, context.sample.touched_stack_bytes, unsigned(guard));
    }
    CHECK(contexts[0].base != contexts[1].base);
    for (auto& context : contexts) context.release = true;
    for (size_t n = 0; n < workers.size(); ++n) {
        REQUIRE(workers[n]->join()); CHECK(!workers[n]->failure());
        void* first = nullptr; void* last = nullptr; uint32_t flags = 0;
        const int query = sceKernelQueryMemoryProtection(reinterpret_cast<void*>(contexts[n].base), &first, &last, &flags);
        CHECK(query != 0 || (flags & 7u) == 0);
    }
}
NXTEST(compact_worker_stacks_preserve_deep_frames_guards_and_release_backing) {
    check_worker_stacks<32u << 20, 24u << 20>();
}
NXTEST(eight_mib_workers_preserve_six_mib_frames_guards_and_release_backing) {
    check_worker_stacks<8u << 20, 6u << 20>();
}
