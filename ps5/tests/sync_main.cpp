// SPDX-License-Identifier: GPL-3.0-or-later
// Runs upstream platform cases and native memory checks against production code.
#include "test_framework.h"
#include "nxrt/sync.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include <cstdio>
#include <exception>
#include <algorithm>
#include <cstring>
#ifdef NXRT_GAME_RENDER_NATIVE
#include "../gfx/game_renderer.h"
#endif
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP) || defined(NXRT_GAME_STARTUP_NATIVE)
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
#ifdef NXRT_GAME_STARTUP_NATIVE
    constexpr const char* prefix = "game-native";
#elif defined(NXRT_TEST_NATIVE_APP)
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
#elif defined(NXRT_TEST_EH)
    constexpr const char* prefix = "eh-runtime";
#elif defined(NXRT_TEST_IPC)
    constexpr const char* prefix = "ipc-runtime";
#elif defined(NXRT_TEST_TIME)
    constexpr const char* prefix = "time-runtime";
#elif defined(NXRT_TEST_AUDIO)
    constexpr const char* prefix = "audio-runtime";
#elif defined(NXRT_TEST_LOADER)
    constexpr const char* prefix = "loader-runtime";
#elif defined(NXRT_GAME_STARTUP_TRACE)
    constexpr const char* prefix = "game-startup";
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
#ifdef NXRT_GAME_RENDER_NATIVE
    // Preserve complete lines for live collection without making every piece
    // of a formatted trace a separate filesystem write. Static buffers outlive
    // all guest workers and the native application's final exit message.
    static char output_buffer[16384], error_buffer[16384];
    if (std::setvbuf(stdout, output_buffer, _IOLBF, sizeof output_buffer)) return 5;
#else
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#endif
    std::puts("Report stream opened");
    if (!std::freopen(report, "a", stderr)) {
        std::printf("Cannot open stderr report: errno=%d\n", errno);
        return 6;
    }
#ifdef NXRT_GAME_RENDER_NATIVE
    if (std::setvbuf(stderr, error_buffer, _IOLBF, sizeof error_buffer)) return 6;
#else
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
    if (setenv("NXRT_TEST_TEMP", "/data/pokopia-recomp/tests", 1) != 0) {
        std::printf("Cannot set test directory: errno=%d\n", errno);
        return 7;
    }
#ifdef NXRT_GAME_RENDER_NATIVE
    if ((mkdir("/data/pokopia-recomp/graphics-cache", 0777) != 0 && errno != EEXIST) ||
        setenv("MESA_SHADER_CACHE_DIR", "/data/pokopia-recomp/graphics-cache", 1) != 0) {
        std::printf("Cannot prepare graphics cache: errno=%d\n", errno);
        return 7;
    }
#endif
#endif
#ifndef NXRT_GAME_RENDER_NATIVE
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#endif
#ifdef NXRT_TEST_NATIVE_APP
    std::puts("context=native-app title=PPSA98731");
#endif
#ifdef NXRT_GAME_STARTUP_NATIVE
#ifdef NXRT_GAME_RENDER_NATIVE
    std::puts("context=native-app title=PPSA98732 diagnostic=game-renderer-startup renderer=true");
#else
    std::puts("context=native-app title=PPSA98732 diagnostic=full-game-startup renderer=false");
#endif
    if (fsync(fileno(stdout))) return 8;
#endif
#ifdef NXRT_GAME_RENDER_NATIVE
    nxrt::log_init({.path = "", .level = nxrt::LogLevel::Info, .stderr_all = true});
#else
    nxrt::log_init({.path = "", .level = nxrt::LogLevel::Error, .to_stderr = false});
#endif
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP) || defined(NXRT_GAME_STARTUP_NATIVE)
    const int64_t limit = sceKernelGetDirectMemorySize();
    int64_t available_at = -1;
    size_t available_bytes = 0;
    const int available_rc = sceKernelAvailableDirectMemorySize(0, limit, 16384, &available_at, &available_bytes);
    std::printf("APP_HEAP direct_limit=%lld available_rc=%08x largest_free=%zu at=%lld\n",
                static_cast<long long>(limit), static_cast<unsigned>(available_rc), available_bytes,
                static_cast<long long>(available_at));
#ifdef NXRT_GAME_STARTUP_NATIVE
    if (fsync(fileno(stdout))) return 8;
#endif
#endif
#ifdef NXRT_GAME_EH
    // Run the exception suite first, independent of static initialization order.
    std::stable_sort(nxtest::cases().begin(), nxtest::cases().end(), [](const auto& a, const auto& b) {
        return (std::strcmp(a.group, "game_images") != 0) > (std::strcmp(b.group, "game_images") != 0);
    });
#endif
    for (const auto& test : nxtest::cases()) {
#ifdef NXRT_GAME_EH
        if (!std::strcmp(test.group, "game_images")) {
            std::printf("GAME_EH_SELFTEST failures=%d\n", nxtest::failures);
            if (nxtest::failures) return 1;
        }
#endif
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
#if defined(NXRT_TEST_NATIVE_APP) || defined(NXRT_TEST_APP_HEAP) || defined(NXRT_GAME_STARTUP_NATIVE)
#ifndef NXRT_GAME_RENDER_NATIVE
    const auto heap = nxrt::app_heap::stats();
    std::printf("APP_HEAP spaces=%zu backing_bytes=%zu live_blocks=%zu live_bytes=%zu\n",
                heap.spaces, heap.backing_bytes, heap.live_blocks, heap.live_bytes);
    std::printf("APP_HEAP last_failure operation=%u rc=%08x bytes=%zu\n",
                heap.failed_operation, static_cast<unsigned>(heap.platform_error), heap.failed_bytes);
#endif
#endif
#ifdef NXRT_GAME_RENDER_NATIVE
    nxrt::ps5_graphics::shutdown();
#endif
    std::printf("%zu cases; %d failures\n", nxtest::cases().size(), nxtest::failures);
    nxrt::log_shutdown();
    return nxtest::failures ? 1 : 0;
}
