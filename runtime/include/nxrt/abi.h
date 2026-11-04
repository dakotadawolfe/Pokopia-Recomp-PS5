// SPDX-License-Identifier: GPL-3.0-or-later
// The generated-code ABI as the runtime sees it. The recompiler owns the header (recomp/include/nxrecomp/ctx.h: Ctx, V128, FnEntry,
// DynSym, NxModule, NxEh and the declarations of the functions this runtime provides: call_indirect, host_import(c, import_id),
// guest_trap, guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag, nx_catch_call, nx_site_inline, nx_site_midhook,
// nx_hook_entry; their definitions are src/core/abi_glue.cpp, src/core/hooks.cpp and src/eh/catch_call.cpp).
// Everything else in nxrt reaches the guest CPU state through the small accessors below.
#pragma once

#include <nxrecomp/ctx.h>

namespace nxrt {

// The ABI version this runtime implements. A lifted module built against another version is refused (nxmodule.h).
#ifdef NXRECOMP_SUBPAGE_CHECKS
// Layout is unchanged from version 3. Version 4 requires checked memory helpers
// so a stale unchecked archive cannot run against mixed guest permissions.
constexpr uint32_t kAbiVersion = 4;
#else
constexpr uint32_t kAbiVersion = 3;
#endif
static_assert(NXRECOMP_ABI_VERSION == kAbiVersion,
              "nxrt and lifted-code ABI contracts differ; review memory helpers and runtime entry points");

using GuestFn = void (*)(::Ctx*);   // a lifted function (or an HLE handler): guest registers in, guest registers out
using HleFn = GuestFn;

// Register access. x[31] is SP; w-register views are the caller's business.
inline uint64_t& reg(::Ctx* c, int n) { return c->x[n]; }
inline uint64_t& sp(::Ctx* c) { return c->x[31]; }
inline uint64_t& lr(::Ctx* c) { return c->x[30]; }
inline uint64_t& fp(::Ctx* c) { return c->x[29]; }
inline uint64_t& tpidr_el0(::Ctx* c) { return c->tpidr; }
inline uint64_t& tpidrro_el0(::Ctx* c) { return c->tpidrro; }

}  // namespace nxrt
