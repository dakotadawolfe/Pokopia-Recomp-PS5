// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/eh: the per-thread stack of catch frames shared by the unwinder (landing) and nx_catch_call (establishing).
#pragma once
#include <setjmp.h>

#include <cstdint>
#include <vector>

#include "nxrt/abi.h"

#ifdef NXRT_TARGET_PS5
// Link the SDK's signal-mask-free implementation into the executable. The
// native companion's compatibility exports are not a working jump context.
extern "C" int nxrt_ps5_setjmp(jmp_buf) __attribute__((returns_twice));
extern "C" [[noreturn]] void nxrt_ps5_longjmp(jmp_buf, int);
#define NXRT_EH_SETJMP nxrt_ps5_setjmp
#define NXRT_EH_LONGJMP nxrt_ps5_longjmp
#else
#define NXRT_EH_SETJMP setjmp
#define NXRT_EH_LONGJMP longjmp
#endif

namespace nxrt::eh::detail {

// One nx_catch_call activation. The unwinder finds the one whose cfa equals the CFA of the frame it lands in, stores the landing pad
// and longjmps to jb; nx_catch_call then calls body(c, pad).
struct CatchRec {
    jmp_buf jb;
    uint64_t cfa = 0;              // guest SP at entry of the function = its CFA
    volatile uint64_t pad = 0;
    volatile bool landed = false;  // set by the unwinder right before the longjmp
};

std::vector<CatchRec*>& catch_stack();   // thread_local; the top is the innermost activation

}  // namespace nxrt::eh::detail
