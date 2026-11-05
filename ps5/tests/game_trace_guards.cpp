// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic-only boundaries: unsupported ABI paths must not return fake data.
#include "nxrt/dispatch.h"
#include "nxrt/nxmodule.h"
#include <cstdio>

extern "C" {
[[noreturn]] void nxrt_game_startup_stop();
uint64_t __real_guest_mrs(Ctx*, uint32_t);
void __real_call_indirect(Ctx*, uint64_t);
void __real_host_import(Ctx*, uint32_t);

uint64_t __wrap_guest_mrs(Ctx* context, uint32_t reg) {
    if (reg == 0xdf00 || reg == 0xdf01 || reg == 0xdf02) return __real_guest_mrs(context, reg);
    std::printf("GAME_STOP unported system register read=%x fn=%llx\n", reg,
        static_cast<unsigned long long>(context->fn));
    nxrt_game_startup_stop();
}
void __wrap_guest_msr(Ctx* context, uint32_t reg, uint64_t value) {
    std::printf("GAME_STOP unported system register write=%x value=%llx fn=%llx\n", reg,
        static_cast<unsigned long long>(value), static_cast<unsigned long long>(context->fn));
    nxrt_game_startup_stop();
}
void __wrap_guest_trap(const char* reason, uint64_t address) {
    std::printf("GAME_STOP guest trap=%s address=%llx\n", reason, static_cast<unsigned long long>(address));
    nxrt_game_startup_stop();
}
void __wrap_guest_unimpl(uint64_t pc, uint32_t instruction, const char* reason) {
    std::printf("GAME_STOP untranslated instruction pc=%llx instruction=%x reason=%s\n",
        static_cast<unsigned long long>(pc), instruction, reason);
    nxrt_game_startup_stop();
}
void __wrap_call_indirect(Ctx* context, uint64_t address) {
    if (!nxrt::fn_lookup(address)) {
        const auto* name = nxrt::hle_name_at(address);
        if (!name || !nxrt::hle_lookup(name)) {
            std::printf("GAME_STOP unbacked indirect call address=%llx fn=%llx lr=%llx symbol=%s\n",
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(context->fn),
                static_cast<unsigned long long>(context->x[30]), name ? name : "(none)");
            nxrt_game_startup_stop();
        }
    }
    __real_call_indirect(context, address);
}
void __wrap_host_import(Ctx* context, uint32_t id) {
    const auto address = nxrt::import_resolve(id);
    if (nxrt::hle_leaf_fn(address)) { __real_host_import(context, id); return; }
    if (!address || (!nxrt::fn_lookup(address) && !nxrt::hle_lookup(nxrt::import_name(id)))) {
        std::printf("GAME_STOP unbacked import id=%x symbol=%s address=%llx fn=%llx\n",
            id, nxrt::import_name(id), static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(context->fn));
        nxrt_game_startup_stop();
    }
    __real_host_import(context, id);
}
}
