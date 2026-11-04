// SPDX-License-Identifier: GPL-3.0-or-later
// Runs upstream platform cases and native memory checks against production code.
#include "test_framework.h"
#include "nxrt/sync.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include <cstdio>
#include <exception>
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP)
#include "../app/heap.h"
extern "C" int64_t sceKernelGetDirectMemorySize();
extern "C" int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
#endif
#ifdef NXRT_TEST_PS5
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nxtest {
static int failures = 0;
std::vector<Case>& cases() { static std::vector<Case> value; return value; }
std::vector<Child>& children() { static std::vector<Child> value; return value; }
void fail(const char* file, int line, const char* expr, const std::string& detail) {
    ++failures;
    std::fprintf(stderr, "%s:%d: %s %s\n", file, line, expr, detail.c_str());
}
void skip(const char* reason) {
    ++failures; // Bring-up requires every selected case to execute.
    std::fprintf(stderr, "Unexpected skipped case: %s\n", reason);
}
}
int main() {
#ifdef NXRT_TEST_PS5
    // Each hardware run keeps a report; individual file tests remove their
    // temporary files. The optional game-image case reads the staged NSOs.
    for (const char* path : {"/data/pokopia-recomp", "/data/pokopia-recomp/tests"}) {
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return 4;
    }
    char report[160];
#ifdef NXRT_TEST_NATIVE_APP
    constexpr const char* prefix = "app-platform";
#elif defined(NXRT_TEST_SHARED_HEAP)
    constexpr const char* prefix = "heap-platform";
#elif defined(NXRT_TEST_MEMORY_API)
    constexpr const char* prefix = "memory-api";
#elif defined(NXRT_TEST_THREAD_EXIT)
    constexpr const char* prefix = "thread-runtime";
#elif defined(NXRT_TEST_KERNEL)
    constexpr const char* prefix = "kernel-runtime";
#elif defined(NXRT_TEST_SVC)
    constexpr const char* prefix = "svc-runtime";
#elif defined(NXRT_TEST_ROMFS)
    constexpr const char* prefix = "romfs-runtime";
#elif defined(NXRT_TEST_FS)
    constexpr const char* prefix = "fs-runtime";
#elif defined(NXRT_TEST_TIME)
    constexpr const char* prefix = "time-runtime";
#elif defined(NXRT_TEST_LOADER)
    constexpr const char* prefix = "loader-runtime";
#elif defined(NXRT_SDK_STARTUP_TRACE)
    constexpr const char* prefix = "sdk-startup";
#elif defined(NXRT_TEST_GAME_IMAGES)
    constexpr const char* prefix = "game-images";
#elif defined(NXRT_TEST_APP_HEAP)
    constexpr const char* prefix = "app-heap";
#else
    constexpr const char* prefix = "platform";
#endif
    std::snprintf(report, sizeof report, "/data/pokopia-recomp/tests/%s-%lld-%d.log", prefix,
                  static_cast<long long>(std::time(nullptr)), static_cast<int>(getpid()));
    // A native application need not start with an open stderr descriptor.
    // Open both streams explicitly and append so their offsets cannot overwrite
    // each other's output. The timestamp/PID names retain earlier runs.
    if (!std::freopen(report, "a", stdout)) return 5;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("Report stream opened");
    if (!std::freopen(report, "a", stderr)) {
        std::printf("Cannot open stderr report: errno=%d\n", errno);
        return 6;
    }
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (setenv("NXRT_TEST_TEMP", "/data/pokopia-recomp/tests", 1) != 0) {
        std::printf("Cannot set test directory: errno=%d\n", errno);
        return 7;
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef NXRT_TEST_NATIVE_APP
    std::puts("context=native-app title=PPSA98731");
#endif
    nxrt::log_init({.path = "", .level = nxrt::LogLevel::Error, .to_stderr = false});
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP)
    const int64_t limit = sceKernelGetDirectMemorySize();
    int64_t available_at = -1;
    size_t available_bytes = 0;
    const int available_rc = sceKernelAvailableDirectMemorySize(0, limit, 16384, &available_at, &available_bytes);
    std::printf("APP_HEAP direct_limit=%lld available_rc=%08x largest_free=%zu at=%lld\n",
                static_cast<long long>(limit), static_cast<unsigned>(available_rc), available_bytes,
                static_cast<long long>(available_at));
#endif
    for (const auto& test : nxtest::cases()) {
        std::printf("RUN %s\n", test.name);
        try { test.fn(); }
        catch (const std::exception& error) {
            ++nxtest::failures;
            std::fprintf(stderr, "FAIL %s: exception: %s\n", test.name, error.what());
        } catch (...) {
            ++nxtest::failures;
            std::fprintf(stderr, "FAIL %s: unknown exception\n", test.name);
        }
        nxrt::sync::reset_all();
    }
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP)
    const auto heap = nxrt::app_heap::stats();
    std::printf("APP_HEAP spaces=%zu backing_bytes=%zu live_blocks=%zu live_bytes=%zu\n",
                heap.spaces, heap.backing_bytes, heap.live_blocks, heap.live_bytes);
    std::printf("APP_HEAP last_failure operation=%u rc=%08x bytes=%zu\n",
                heap.failed_operation, static_cast<unsigned>(heap.platform_error), heap.failed_bytes);
#endif
    std::printf("%zu cases; %d failures\n", nxtest::cases().size(), nxtest::failures);
    nxrt::log_shutdown();
    return nxtest::failures ? 1 : 0;
}
