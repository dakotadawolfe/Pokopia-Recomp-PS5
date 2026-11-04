// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int pkp_graphics_display_probe();

int main() {
    for (const char* path : {"/data/pokopia-recomp", "/data/pokopia-recomp/tests", "/data/pokopia-recomp/graphics-cache"})
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return 4;
    char report[192];
    std::snprintf(report, sizeof report, "/data/pokopia-recomp/tests/graphics-display-%lld-%d.log",
        static_cast<long long>(std::time(nullptr)), static_cast<int>(getpid()));
    if (!std::freopen(report, "a", stdout)) return 5;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!std::freopen(report, "a", stderr)) { std::printf("GRAPHICS_FAIL operation=stderr errno=%d\n", errno); return 6; }
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("context=native-app diagnostic=graphics title=PPSA98732 pid=%d\n", static_cast<int>(getpid()));
    // This process owns no guest address ranges. Mesa's cache stays in the
    // diagnostic's data directory rather than its mounted application folder.
    if (setenv("MESA_SHADER_CACHE_DIR", "/data/pokopia-recomp/graphics-cache", 1) != 0) {
        std::printf("GRAPHICS_FAIL operation=cache-directory errno=%d\n", errno);
        return 7;
    }
    const int result = pkp_graphics_display_probe();
    std::printf("graphics diagnostic returned=%d; no gameplay tested\n", result);
    return result;
}
