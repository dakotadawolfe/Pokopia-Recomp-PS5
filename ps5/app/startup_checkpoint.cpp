// SPDX-License-Identifier: GPL-3.0-or-later
// Entry checkpoints use kernel file operations before libc or C++ constructors.
#include <cstddef>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
void __real__init_env(void*);
int sceKernelOpen(const char*, int, mode_t);
ssize_t sceKernelWrite(int, const void*, size_t);
int sceKernelFsync(int);
int sceKernelClose(int);
}

namespace {
void checkpoint(const char* message, size_t length) {
    const int fd = sceKernelOpen("/data/pokopia-recomp/tests/native-bootstrap.log",
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char suffix[32];
    size_t count = 0;
    unsigned pid = static_cast<unsigned>(getpid());
    do { suffix[count++] = char('0' + pid % 10); pid /= 10; } while (pid && count < 30);
    for (size_t i = 0; i < count / 2; ++i) {
        const char other = suffix[count - 1 - i];
        suffix[count - 1 - i] = suffix[i]; suffix[i] = other;
    }
    suffix[count++] = '\n';
    if (sceKernelWrite(fd, message, length) == static_cast<ssize_t>(length) &&
        sceKernelWrite(fd, suffix, count) == static_cast<ssize_t>(count)) (void)sceKernelFsync(fd);
    (void)sceKernelClose(fd);
}
}

extern "C" void __wrap__init_env(void* parameters) {
    constexpr char before[] = "STARTUP_BOOTSTRAP enter_init_env pid=";
    constexpr char after[] = "STARTUP_BOOTSTRAP after_init_env pid=";
    checkpoint(before, sizeof before - 1);
    __real__init_env(parameters);
    checkpoint(after, sizeof after - 1);
}
