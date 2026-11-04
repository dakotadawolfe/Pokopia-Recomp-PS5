// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include <setjmp.h>
NXTEST_GROUP(host_jumps)

namespace {
[[noreturn, gnu::noinline]] void leave_frame(jmp_buf saved, int result, volatile int* marker) {
    *marker = 73;
    longjmp(saved, result);
}
}

NXTEST(shader_compiler_nonlocal_jumps_restore_the_calling_frame) {
    // These standard names are the same imports RADV uses, independently of
    // the private names exercised by the guest exception suite.
    for (int requested : {0, 23}) {
        jmp_buf saved;
        volatile int marker = 0;
        switch (setjmp(saved)) {
        case 0: leave_frame(saved, requested, &marker);
        case 1: REQUIRE(requested == 0); break;
        case 23: REQUIRE(requested == 23); break;
        default: REQUIRE(false);
        }
        REQUIRE(marker == 73);
    }
}
