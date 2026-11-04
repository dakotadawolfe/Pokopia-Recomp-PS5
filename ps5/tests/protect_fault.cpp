// SPDX-License-Identifier: GPL-3.0-or-later
#include "protect_fault.h"
#include <cerrno>
#include <cstddef>

std::atomic<unsigned> test_fail_protect{0};
extern "C" int __real_mprotect(void*, size_t, int);
extern "C" int __wrap_mprotect(void* address, size_t size, int protection) {
    unsigned value = test_fail_protect.load();
    while (value && !test_fail_protect.compare_exchange_weak(value, value - 1)) {}
    if (value == 1) { errno = ENOMEM; return -1; }
    return __real_mprotect(address, size, protection);
}
