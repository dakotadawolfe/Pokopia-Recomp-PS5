// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the upstream synchronization cases against the production platform code.
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
    std::snprintf(report, sizeof report, "/data/pokopia-recomp/tests/platform-%lld-%d.log",
                  static_cast<long long>(std::time(nullptr)), static_cast<int>(getpid()));
    if (!std::freopen(report, "w", stdout)) return 5;
    if (dup2(fileno(stdout), fileno(stderr)) < 0) return 6;
    if (setenv("NXRT_TEST_TEMP", "/data/pokopia-recomp/tests", 1) != 0) return 7;
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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
