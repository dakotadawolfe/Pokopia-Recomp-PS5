// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
extern int sceLncUtilGetAppTitleId(uint32_t, char*);
extern int sceLncUtilKillApp(uint32_t);

int main(void) {
    char report[180];
    snprintf(report, sizeof report, "/data/pokopia-recomp/tests/native-close-%lld-%d.log",
             (long long)time(NULL), getpid());
    if (!freopen(report, "a", stdout)) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t app = sceLncUtilGetAppIdOfRunningBigApp();
    if (!app || app == UINT32_MAX) {
        puts("NATIVE_CLOSE no active application");
        return 0;
    }
    char title[32] = {0};
    if (sceLncUtilGetAppTitleId(app, title) || strcmp(title, "PPSA98732") ||
        sceLncUtilGetAppIdOfRunningBigApp() != app) {
        printf("NATIVE_CLOSE refused app=%08x title=%s\n", app, title);
        return 2;
    }
    printf("NATIVE_CLOSE requesting app=%08x title=%s\n", app, title);
    if (fsync(fileno(stdout))) return 3;
    const int result = sceLncUtilKillApp(app);
    printf("NATIVE_CLOSE result=%08x; verify process exit and unmount separately\n", result);
    if (fsync(fileno(stdout))) return 3;
    return result < 0 ? 4 : 0;
}
