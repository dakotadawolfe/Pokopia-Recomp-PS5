// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrecomp/ctx.h"
#include "nxrt/log.h"

// Called on every failed exclusive store; reports a loop that never succeeds.
extern "C" void stx_diag(Ctx* c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    uint64_t n = ++c->ex_fail;
    if (n == 1000 || n == 100000)
        NXLOG_W("guest", "STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(a), static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(seen), static_cast<unsigned long long>(v), bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
}
