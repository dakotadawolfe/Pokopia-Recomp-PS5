// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>

extern "C" {
void nxrt_ps5_finish_thread();
int sceSystemServiceLoadExec(const char* path, const char** arguments);
int sceKernelUsleep(uint32_t microseconds);
}

// Native applications must ask the shell to close them. Returning to this
// toolchain's exit() path raises SIGSYS even when main returned zero.
extern "C" [[noreturn]] void catchReturnFromMain(int status) {
    nxrt_ps5_finish_thread();
    std::printf("native-app main returned status=%d; requesting system exit\n", status);
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec("exit", nullptr);
    std::fprintf(stderr, "native-app system exit returned rc=0x%08x; waiting for shell close\n",
                 static_cast<unsigned>(result));
    std::fflush(nullptr);
    // A refused request must not fall through to the invalid exit syscall.
    // The report retains the test result, and Options -> Close Game can stop it.
    for (;;) sceKernelUsleep(100000);
}
