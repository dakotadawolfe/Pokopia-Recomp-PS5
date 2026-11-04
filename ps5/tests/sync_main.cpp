// SPDX-License-Identifier: GPL-3.0-or-later
// Runs upstream platform cases and native memory checks against production code.
#include "test_framework.h"
#include "nxrt/sync.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include <cstdio>
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
    // temporary files. No game or emulator files are accessed by this suite.
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
#elif defined(NXRT_TEST_LOADER)
    constexpr const char* prefix = "loader-runtime";
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
    for (const auto& test : nxtest::cases()) {
        std::printf("RUN %s\n", test.name);
        test.fn();
        nxrt::sync::reset_all();
    }
    std::printf("%zu cases; %d failures\n", nxtest::cases().size(), nxtest::failures);
    nxrt::log_shutdown();
    return nxtest::failures ? 1 : 0;
}
