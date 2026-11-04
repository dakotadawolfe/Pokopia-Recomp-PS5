// SPDX-License-Identifier: GPL-3.0-or-later
// One-shot launcher for this repository's installed diagnostic title only.
// Launch ABI follows the pinned PS5 Vulkan payload/ps5vkctl implementation.
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
typedef struct { uint32_t size; int user_id; uint32_t app_opt; uint64_t crash_report, check_flag; } Launch;
extern uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
extern int sceLncUtilGetAppTitleId(uint32_t, char*);
extern int sceLncUtilLaunchApp(const char*, const char*[], Launch*);
extern int sceUserServiceInitialize(void*);
extern int sceUserServiceGetForegroundUser(int*);
extern int sceUserServiceGetInitialUser(int*);
int main(void) {
    char report[180];
    snprintf(report, sizeof report, "/data/pokopia-recomp/tests/native-launch-%lld-%d.log", (long long)time(NULL), getpid());
    if (!freopen(report, "a", stdout)) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t app = sceLncUtilGetAppIdOfRunningBigApp();
    if (app && app != UINT32_MAX) {
        char title[32] = {0}; sceLncUtilGetAppTitleId(app, title);
        printf("NATIVE_LAUNCH refused active_app=%08x title=%s\n", app, title); return 2;
    }
    int user = -1; int init = sceUserServiceInitialize(NULL);
    int user_result = sceUserServiceGetForegroundUser(&user);
    if (user_result || user < 0) user_result = sceUserServiceGetInitialUser(&user);
    if (user_result || user < 0) { printf("NATIVE_LAUNCH no_user init=%08x result=%08x\n", init, user_result); return 3; }
    Launch launch = {.size=sizeof(Launch), .user_id=user};
    const char* args[] = {NULL};
    int result = sceLncUtilLaunchApp("PPSA98732", args, &launch);
    printf("NATIVE_LAUNCH title=PPSA98732 user=%x result=%08x; verify app log separately\n", user, result);
    return result < 0 ? 4 : 0;
}
