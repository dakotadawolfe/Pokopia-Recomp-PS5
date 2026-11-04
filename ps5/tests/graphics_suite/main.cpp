// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int pkp_graphics_display_probe();
#ifdef PKP_GRAPHICS_MEMORY_TEST
extern "C" int pkp_graphics_memory_probe();
#elif defined(PKP_GRAPHICS_RUNTIME_TEST)
extern "C" int pkp_graphics_runtime_probe();
#endif

int main() {
    for (const char* path : {"/data/pokopia-recomp", "/data/pokopia-recomp/tests", "/data/pokopia-recomp/graphics-cache"})
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return 4;
    char report[192];
#ifdef PKP_GRAPHICS_MEMORY_TEST
    constexpr const char* report_format = "/data/pokopia-recomp/tests/graphics-memory-%lld-%d.log";
#elif defined(PKP_GRAPHICS_RUNTIME_TEST)
    constexpr const char* report_format = "/data/pokopia-recomp/tests/graphics-runtime-%lld-%d.log";
#else
    constexpr const char* report_format = "/data/pokopia-recomp/tests/graphics-display-%lld-%d.log";
#endif
    std::snprintf(report, sizeof report, report_format,
        static_cast<long long>(std::time(nullptr)), static_cast<int>(getpid()));
    if (!std::freopen(report, "a", stdout)) return 5;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!std::freopen(report, "a", stderr)) { std::printf("GRAPHICS_FAIL operation=stderr errno=%d\n", errno); return 6; }
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("context=native-app diagnostic=graphics title=PPSA98732 pid=%d\n", static_cast<int>(getpid()));
    // Mesa's cache stays in the diagnostic's data directory rather than its
    // mounted application folder. This executable contains no game code.
    if (setenv("MESA_SHADER_CACHE_DIR", "/data/pokopia-recomp/graphics-cache", 1) != 0) {
        std::printf("GRAPHICS_FAIL operation=cache-directory errno=%d\n", errno);
        return 7;
    }
#ifdef PKP_GRAPHICS_MEMORY_TEST
    const int result = pkp_graphics_memory_probe();
#elif defined(PKP_GRAPHICS_RUNTIME_TEST)
    const int result = pkp_graphics_runtime_probe();
#else
    const int result = pkp_graphics_display_probe();
#endif
    std::printf("graphics diagnostic returned=%d; no gameplay tested\n", result);
    return result;
}
