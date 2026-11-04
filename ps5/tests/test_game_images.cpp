// SPDX-License-Identifier: GPL-3.0-or-later
// Checks supplied images and optionally runs their rtld up to the SDK boundary.
#include "test_framework.h"
#include "private_game_images.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/dispatch.h"
#ifdef NXRT_GAME_RENDER_NATIVE
#include "../gfx/game_renderer.h"
#endif
#include "trace_ipc_reply.h"
#ifdef NXRT_GAME_EH
#include "nxrt/eh.h"
#include "nxrt/hooks.h"
#endif
#ifdef NXRT_GAME_STARTUP_TRACE
#include "private_startup_inputs.h"
#endif
#ifdef NXRT_GAME_STARTUP_NATIVE
#include "../app/folder_picker.h"
#include "fs_romfs.h"
#include <unistd.h>
extern "C" [[noreturn]] void catchReturnFromMain(int);
extern "C" int64_t sceKernelGetDirectMemorySize();
extern "C" int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#ifdef NXRT_TEST_RTLD
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services.h"
#include "nxrt/ipc.h"
#include "nxrt/hipc.h"
#include "nxrt/subpage.h"
#include "nxrt/svc.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
namespace nxrt { void svc_memory_reset_for_tests(); void svc_info_reset_for_tests(); }
namespace nxrt::services { void register_logfault(); void logfault_reset(); void register_am(); void am_reset(); }
namespace nxrt::services { void register_aoc(); void aoc_reset(); }
namespace nxrt::services { void register_pctl(); }
namespace nxrt::services { void register_fs(); void fs_reset(); }
namespace nxrt::services { void register_time(); void time_reset(); }
#endif

NXTEST_GROUP(game_images)
namespace {
namespace mem = nxrt::mem;
constexpr auto crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
        table[i] = value;
    }
    return table;
}


constexpr auto table = crc_table();
uint32_t crc_update(uint32_t value, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) value = table[(value ^ data[i]) & 255] ^ (value >> 8);
    return value;
}
[[maybe_unused]] uint32_t crc(const uint8_t* data, size_t size) { return crc_update(~0u, data, size) ^ ~0u; }
bool check_file(const std::string& path, const GameImageInput& expected) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(expected.bytes)) return false;
    file.seekg(0);
    std::array<uint8_t, 65536> buffer{};
    file.read(reinterpret_cast<char*>(buffer.data()), sizeof expected.header);
    if (!file || std::memcmp(buffer.data(), expected.header, sizeof expected.header)) return false;
    file.seekg(0);
    uint32_t value = ~0u;
    uint64_t left = expected.bytes;
    while (left) {
        const auto size = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        if (!file) return false;
        value = crc_update(value, buffer.data(), size);
        left -= size;
    }
    return (value ^ ~0u) == expected.crc32;
}
struct Cleanup {
    ~Cleanup() { nxrt::modules_unload_all(); nxrt::hle_clear(); mem::shutdown(); }
};
}

#ifndef NXRT_GAME_STARTUP_TRACE
NXTEST(decrypted_game_images_map_with_original_contents_and_permissions) {
    Cleanup cleanup;
    nxrt::modules_unload_all(); mem::shutdown();
    REQUIRE(mem::init({.heap_reserve = 12ull << 30}));
    REQUIRE(mem::heap_create(12ull << 30));
    // The oracle is produced with Python lz4 + hashlib + zlib, independently of
    // the runtime's NSO decompressor. SHA-256 also verifies staging/readback.
    CHECK_EQ(crc(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xcbf43926u);
    for (const auto& expected : game_image_inputs) {
        std::printf("GAME_IMAGE loading %s at %llx\n", expected.name,
                    static_cast<unsigned long long>(expected.base));
        const auto path = std::string("/data/pokopia-recomp/game/exefs/") + expected.name;
        REQUIRE(check_file(path, expected));
        nxrt::LoadOptions options;
        options.name = expected.name; options.base = expected.base;
        std::string error;
        auto* module = nxrt::module_load_nso_file(path.c_str(), options, &error);
        if (!module) std::printf("GAME_IMAGE load failed: %s\n", error.c_str());
        REQUIRE(module);
        CHECK_EQ(module->base(), expected.base);
        const auto& data = expected.segments[2];
        CHECK_EQ(module->size(), (data.offset + data.bytes + expected.bss + mem::kPage - 1) & ~(mem::kPage - 1));
        CHECK(!module->symbols().empty());
        for (const auto& segment : expected.segments) {
            const auto* address = reinterpret_cast<const uint8_t*>(expected.base + segment.offset);
            CHECK_EQ(crc(address, segment.bytes), segment.crc32);
        }
        const auto* bss = reinterpret_cast<const uint8_t*>(expected.base + data.offset + data.bytes);
        CHECK(std::all_of(bss, bss + expected.bss, [](uint8_t byte) { return byte == 0; }));
        std::printf("GAME_IMAGE mapped %s bytes=%llu symbols=%zu; contents checked\n", expected.name,
                    static_cast<unsigned long long>(module->size()), module->symbols().size());
    }
    CHECK_EQ(nxrt::modules().size(), size_t{4});
    // Pokopia's own rtld will relocate its images later. Keep the original
    // contents here; module_link_all and guest entry points are not called.
    REQUIRE(nxrt::module_protect_all());
    for (const auto& expected : game_image_inputs) {
        constexpr uint32_t permissions[] = {5, 1, 3};
        for (unsigned i = 0; i < 3; ++i) {
            const auto& segment = expected.segments[i];
            const uint64_t address = expected.base + segment.offset;
            CHECK_EQ(mem::vm_query(address).perm, permissions[i]);
            CHECK(mem::is_mapped(address, segment.bytes));
            CHECK_EQ(crc(reinterpret_cast<const uint8_t*>(address), segment.bytes), segment.crc32);
        }
        CHECK(!nxrt::module_by_name(expected.name)->linked());
    }
    nxrt::modules_unload_all();
    for (const auto& expected : game_image_inputs) {
        CHECK(!mem::is_mapped(expected.base, mem::kPage));
        REQUIRE(mem::reserve_fixed(expected.base, mem::kAllocGranularity, "game image released"));
        CHECK(mem::release_fixed(expected.base));
    }
    std::puts("GAME_IMAGE four images checked and unloaded; no guest code executed");
}
#endif

#ifdef NXRT_TEST_RTLD
namespace {
extern "C" const NxModule nxmod_rtld;
#ifdef NXRT_TEST_SDK
extern "C" const NxModule nxmod_sdk;
extern "C" const uint32_t nxmod_sdk_abi;
#endif
#ifdef NXRT_GAME_STARTUP_TRACE
extern "C" const NxModule nxmod_main, nxmod_subsdk0;
extern "C" const uint32_t nxmod_main_abi, nxmod_subsdk0_abi, nxmod_rtld_abi;
constexpr bool game_startup_trace = true;
#else
constexpr bool game_startup_trace = false;
#endif
struct Handoff {};
struct ProbeStop {};
#ifdef NXRT_SDK_STARTUP_TRACE
constexpr bool startup_trace = true;
#else
constexpr bool startup_trace = false;
#endif
#ifdef NXRT_GAME_RENDER_NATIVE
void report_startup_profile(bool detailed);
#endif
[[noreturn]] void stop_probe() {
#ifdef NXRT_GAME_RENDER_NATIVE
    report_startup_profile(true);
    nxrt::ps5_graphics::report();
#endif
#ifdef NXRT_GAME_EH
    const auto eh = nxrt::eh::stats();
    std::printf("GAME_EH raised=%llu caught=%llu cleanups=%llu uncaught=%llu landings=%llu\n",
        static_cast<unsigned long long>(eh.raised), static_cast<unsigned long long>(eh.caught),
        static_cast<unsigned long long>(eh.cleanups), static_cast<unsigned long long>(eh.uncaught),
        static_cast<unsigned long long>(eh.catch_frame_landings));
#endif
    if constexpr (startup_trace) {
        std::puts(game_startup_trace ?
            "GAME_TRACE_END stopped at dependency or diagnostic limit; not a passing test or gameplay result" :
            "SDK_TRACE_END stopped at dependency or diagnostic limit; not a passing test or gameplay result");
        std::fflush(stdout); std::fflush(stderr);
#ifdef NXRT_GAME_STARTUP_NATIVE
        (void)fsync(fileno(stdout));
        catchReturnFromMain(2); // Native applications must ask the shell to exit.
#else
        std::_Exit(2); // Ends this diagnostic process and all of its guest workers.
#endif
    }
    throw ProbeStop{};
}
std::mutex sdk_ipc_probe_mutex;
std::array<nxrt::SvcHandler, nxrt::kSvcCount> rtld_handlers{};
std::atomic<unsigned> rtld_calls{0};
#ifdef NXRT_GAME_RENDER_NATIVE
// Fixed storage keeps the measurements independent of the guest and renderer
// allocators. Times are summed across workers, so they are not CPU percentages.
struct StartupSvcStats {
    std::atomic<uint64_t> entered{0}, completed{0}, active{0}, total_ns{0}, handler_ns{0}, max_ns{0};
    std::atomic<uint64_t> last_fn{0}, last_lr{0};
};
std::array<StartupSvcStats, nxrt::kSvcCount> startup_svc_stats;
std::atomic<uint64_t> startup_started_ns{0};
uint64_t startup_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct StartupSvcScope {
    StartupSvcStats& stats;
    const uint64_t started = startup_now_ns();
    uint64_t handler_started = 0, handler_elapsed = 0;
    bool first;
    StartupSvcScope(size_t number, const Ctx* c) : stats(startup_svc_stats[number]),
        first(stats.entered.fetch_add(1, std::memory_order_relaxed) == 0) {
        stats.active.fetch_add(1, std::memory_order_relaxed);
        stats.last_fn.store(c->fn, std::memory_order_relaxed);
        stats.last_lr.store(c->x[30], std::memory_order_relaxed);
    }
    void begin_handler() { handler_started = startup_now_ns(); }
    void end_handler() {
        handler_elapsed += startup_now_ns() - handler_started;
        handler_started = 0;
    }
    ~StartupSvcScope() {
        const auto now = startup_now_ns();
        const auto elapsed = now - started;
        if (handler_started) handler_elapsed += now - handler_started;
        stats.total_ns.fetch_add(elapsed, std::memory_order_relaxed);
        stats.handler_ns.fetch_add(handler_elapsed, std::memory_order_relaxed);
        auto maximum = stats.max_ns.load(std::memory_order_relaxed);
        while (maximum < elapsed && !stats.max_ns.compare_exchange_weak(maximum, elapsed, std::memory_order_relaxed)) {}
        stats.completed.fetch_add(1, std::memory_order_relaxed);
        stats.active.fetch_sub(1, std::memory_order_relaxed);
    }
};
void report_startup_profile(bool detailed) {
    uint64_t completed = 0, active = 0, elapsed = 0, handler = 0;
    for (size_t n = 0; n < startup_svc_stats.size(); ++n) {
        auto& s = startup_svc_stats[n];
        const auto count = s.completed.load(std::memory_order_relaxed);
        const auto running = s.active.load(std::memory_order_relaxed);
        const auto total = s.total_ns.load(std::memory_order_relaxed);
        const auto service = s.handler_ns.load(std::memory_order_relaxed);
        completed += count; active += running; elapsed += total; handler += service;
        if (detailed && s.entered.load(std::memory_order_relaxed))
            std::printf("GAME_SVC_PROFILE svc=%02zx completed=%llu active=%llu handler_ms=%llu observer_ms=%llu max_ms=%llu last_fn=%llx last_lr=%llx\n",
                n, static_cast<unsigned long long>(count), static_cast<unsigned long long>(running),
                static_cast<unsigned long long>(service / 1000000),
                static_cast<unsigned long long>((total > service ? total - service : 0) / 1000000),
                static_cast<unsigned long long>(s.max_ns.load(std::memory_order_relaxed) / 1000000),
                static_cast<unsigned long long>(s.last_fn.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.last_lr.load(std::memory_order_relaxed)));
    }
    const auto began = startup_started_ns.load(std::memory_order_relaxed);
    std::printf("GAME_STARTUP_PROGRESS elapsed_ms=%llu calls=%u completed=%llu active=%llu handler_ms=%llu observer_ms=%llu\n",
        static_cast<unsigned long long>(began ? (startup_now_ns() - began) / 1000000 : 0), rtld_calls.load(),
        static_cast<unsigned long long>(completed), static_cast<unsigned long long>(active),
        static_cast<unsigned long long>(handler / 1000000),
        static_cast<unsigned long long>((elapsed > handler ? elapsed - handler : 0) / 1000000));
}
#endif
uint64_t rtld_handoff_handle = 0;
bool sdk_map_boundary = false, sdk_thread_boundary = false;
uint64_t sdk_stack_address = 0;
uint32_t sdk_worker_handle = 0;
bool sdk_worker_wait = false;
#ifdef NXRT_TEST_SDK
nxrt::GuestFn sdk_start = nullptr;
std::array<nxrt::GuestFn, 3> sdk_callbacks{};
std::array<unsigned, 3> sdk_callback_returns{};
#ifdef NXRT_GAME_STARTUP_TRACE
nxrt::GuestFn game_start = nullptr;
void trace_game_start(::Ctx* c) {
    std::puts("GAME_ENTER nninitStartup; executing registered main code");
    game_start(c);
    std::puts("GAME_RETURN nninitStartup");
}
bool register_game_modules() {
    const NxModule* descriptors[] = {&nxmod_main, &nxmod_sdk, &nxmod_subsdk0, &nxmod_rtld};
    const uint32_t versions[] = {nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_rtld_abi};
    static_assert(std::size(descriptors) == std::size(startup_module_inputs));
    std::printf("GAME_INPUT stamp=%s\n", startup_input_stamp);
    for (size_t n = 0; n < std::size(descriptors); ++n) {
        const auto* nx = descriptors[n];
        const auto& expected = startup_module_inputs[n];
        const auto* image = nxrt::module_by_name(expected.name);
        if (!image || std::strcmp(nx->name, expected.name) || nx->base != expected.base ||
            nx->index != expected.index || nx->nfns != expected.functions || versions[n] != expected.abi) return false;
        const auto result = nxrt::nxmodule_register(nxrt::module_by_name(expected.name), nx, versions[n]);
        if (!result.ok() || result.functions != static_cast<size_t>(nx->nfns)) {
            std::printf("GAME_STOP registration module=%s reason=%s count=%zu/%d\n",
                expected.name, result.message.c_str(), result.functions, nx->nfns);
            return false;
        }
        std::printf("GAME_MODULE name=%s index=%u abi=%u functions=%d source_sha256=%s archive_sha256=%s\n",
            expected.name, nx->index, versions[n], nx->nfns, expected.source_sha256, expected.archive_sha256);
    }
    // Bind imports to real providers. Guest unwinding is handled by entry hooks
    // below, including calls emitted directly inside the SDK translation unit.
    for (const auto* nx : descriptors) {
        size_t resolved = 0, missing = 0;
        for (int n = 0; n < nx->nsyms; ++n) {
            const auto& symbol = nx->syms[n];
            if (symbol.defined || !symbol.name || !*symbol.name) continue;
            if (nxrt::module_symbol_overridden(symbol.name)) return false;
            auto target = nxrt::nxmodule_find_export(symbol.name, nx);
            if (!target) {
                const auto mapped = nxrt::module_find_symbol(symbol.name);
                if (mapped && nxrt::fn_lookup(mapped)) target = mapped;
            }
            if (target) {
                if (nxrt::import_resolve(NX_IMPORT_ID(nx->index, n)) != target || nxrt::hle_is_thunk(target)) return false;
                ++resolved;
            } else {
                nxrt::hle_register(symbol.name, +[](::Ctx* c) {
                    std::printf("GAME_STOP unresolved import=%s fn=%llx lr=%llx\n", c->in_import,
                        static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]));
                    stop_probe();
                });
                ++missing;
            }
        }
        std::printf("GAME_BIND module=%s resolved=%zu missing=%zu; missing imports stop only if called\n", nx->name, resolved, missing);
    }
    const auto entry = nxrt::nxmodule_find_export("nninitStartup");
    game_start = nxrt::fn_lookup(entry);
    if (!entry || !game_start || nxrt::nxmodule_at(entry) != &nxmod_main) return false;
    nxrt::fn_register(entry, trace_game_start);
    std::printf("GAME_BIND nninitStartup=%llx module=main\n", static_cast<unsigned long long>(entry));
#ifdef NXRT_GAME_EH
    nxrt::hooks_clear();
    nxrt::eh::set_abi_autodetect(); nxrt::eh::stats_reset();
    nxrt::eh::set_typeinfo_vtables({});
    nxrt::eh::set_terminate_handler(+[](::Ctx*, const char* why) {
        std::printf("GAME_STOP guest terminate=%s\n", why); stop_probe();
    });
    const struct { const char* name; uint64_t address; nxrt::SiteHookFn hook; } hooks[] = {
        {"_Unwind_RaiseException", 0x750078f440, nxrt::eh::hook_unwind_raise_exception},
        {"_Unwind_Resume", 0x750078f660, nxrt::eh::hook_unwind_resume},
        {"_Unwind_Resume_or_Rethrow", 0x750078f940, nxrt::eh::hook_unwind_resume_or_rethrow},
        {"_Unwind_DeleteException", 0x750078f8a0, nxrt::eh::hook_unwind_delete_exception},
        {"_Unwind_ForcedUnwind", 0x750078f7e0, +[](::Ctx*, uint64_t, void*) -> int {
            std::puts("GAME_STOP forced unwinding is unsupported"); stop_probe();
        }},
    };
    for (const auto& hook : hooks) {
        const auto address = nxrt::nxmodule_find_export(hook.name);
        if (address != hook.address || nxrt::nxmodule_at(address) != &nxmod_sdk ||
            !nxrt::hook_add_entry(address, hook.hook)) return false;
    }
    const auto vtables = nxrt::eh::discover_typeinfo_vtables();
    std::printf("GAME_EH_READY hooks=%zu typeinfo_vtables=%zu\n", std::size(hooks), vtables);
    if (vtables != 9 || nxrt::eh::catch_frame_depth()) return false;
#endif
    return true;
}
#endif
template<unsigned N> void sdk_callback(::Ctx* c) {
    std::printf("SDK_CALLBACK begin=%u\n", N); sdk_callbacks[N](c); ++sdk_callback_returns[N];
    std::printf("SDK_CALLBACK returned=%u\n", N);
}
void trace_sdk_start(::Ctx* c) {
    rtld_handoff_handle = c->x[0];
    constexpr nxrt::GuestFn wrappers[] = {sdk_callback<0>, sdk_callback<1>, sdk_callback<2>};
    for (unsigned n = 0; n < 3; ++n) {
        sdk_callbacks[n] = nxrt::fn_lookup(c->x[n + 2]); sdk_callback_returns[n] = 0;
        if (!sdk_callbacks[n]) stop_probe();
        nxrt::fn_register(c->x[n + 2], wrappers[n]);
    }
    std::printf("SDK_ENTER main_handle=%llx\n", static_cast<unsigned long long>(rtld_handoff_handle));
    sdk_start(c);
}
#endif
template<size_t Number> void rtld_svc(::Ctx* c) {
    const unsigned call_number = ++rtld_calls;
#ifdef NXRT_GAME_RENDER_NATIVE
    StartupSvcScope profile(Number, c);
    const bool trace_call = profile.first;
#else
    const bool trace_call = startup_trace || call_number <= 100;
#endif
    if (call_number > (game_startup_trace ? 10000u : startup_trace ? 1000u : 10000u)) { std::puts("RTLD_STOP system-call limit"); stop_probe(); }
    const uint64_t a = c->x[0], b = c->x[1], d = c->x[2];
#ifdef NXRT_GAME_STARTUP_NATIVE
    if constexpr (Number == 0x01) {
        const auto limit = sceKernelGetDirectMemorySize();
        int64_t at = -1; size_t bytes = 0;
        const int rc = sceKernelAvailableDirectMemorySize(0, limit, 16384, &at, &bytes);
        std::printf("GAME_HEAP requested=%llu native_limit=%lld largest_free=%zu query_rc=%08x diagnostic_limit_mib=%u\n",
            static_cast<unsigned long long>(b), static_cast<long long>(limit), bytes,
            static_cast<unsigned>(rc), unsigned(PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB));
        if (fsync(fileno(stdout))) {
            std::puts("GAME_STOP cannot persist memory checkpoint"); stop_probe();
        }
        if (b > (uint64_t{PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB} << 20)) {
            std::puts("GAME_STOP heap request exceeds diagnostic threshold; no allocation attempted");
            stop_probe();
        }
        const uint64_t committed = mem::heap_committed();
        const uint64_t growth = b > committed ? b - committed : 0;
        constexpr uint64_t host_headroom = uint64_t{1} << 30;
        std::printf("GAME_HEAP_BUDGET guest_total=%llu committed=%llu growth=%llu projected_free=%llu minimum_free=%llu\n",
            static_cast<unsigned long long>(nxrt::svc_total_memory()), static_cast<unsigned long long>(committed),
            static_cast<unsigned long long>(growth), static_cast<unsigned long long>(bytes > growth ? bytes - growth : 0),
            static_cast<unsigned long long>(host_headroom));
        if (growth && (limit <= 0 || rc || bytes < host_headroom || growth > bytes - host_headroom)) {
            std::puts("GAME_STOP native free-memory check cannot retain 1 GiB host headroom; no allocation attempted");
            stop_probe();
        }
    }
#endif
    if (startup_trace && trace_call) {
        std::printf("SDK_TRACE_ENTER n=%u svc=%02zx thread=%x fn=%llx lr=%llx a=%llx b=%llx d=%llx\n",
            call_number, Number, nxrt::kernel::current_thread() ? nxrt::kernel::current_thread()->handle : 0,
            static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(d));
    }
    // Keep diagnostic request inspection and domain updates in the same order.
    // This trace is for startup dependencies, not performance measurement.
    std::unique_lock ipc_lock(sdk_ipc_probe_mutex, std::defer_lock);
    if constexpr (Number == 0x21 || Number == 0x22) ipc_lock.lock();
#ifdef NXRT_TEST_SDK
    if constexpr (!startup_trace && Number == 0x04) {
        const auto* sdk = nxrt::module_by_name("sdk");
        if (!sdk || b != sdk->base() + 0xe14000 || d != mem::kPage ||
            (a & (mem::kPage - 1)) || a < mem::kStackBase || a - mem::kStackBase > mem::kStackSize - d ||
            !mem::SubpageMap::allows(b, d, 3) || mem::is_mapped(a, d)) {
            std::puts("SDK_STOP unexpected stack mapping request"); stop_probe();
        }
        sdk_stack_address = a;
    }
    if constexpr (!startup_trace && Number == 0x08) {
        std::printf("SDK_THREAD entry=%llx arg=%llx sp=%llx priority=%llx core=%llx\n",
            static_cast<unsigned long long>(b), static_cast<unsigned long long>(d),
            static_cast<unsigned long long>(c->x[3]), static_cast<unsigned long long>(c->x[4]),
            static_cast<unsigned long long>(c->x[5]));
        if (!sdk_map_boundary || !nxrt::fn_lookup(b) || (c->x[3] & 15) || c->x[3] <= sdk_stack_address || c->x[3] > sdk_stack_address + mem::kPage ||
            !mem::SubpageMap::allows(sdk_stack_address, mem::kPage, 3)) stop_probe();
    }
    if constexpr (!startup_trace && Number == 0x1a) {
        const auto* worker = nxrt::kernel::current_thread();
        if (!worker || worker->handle != sdk_worker_handle || a != rtld_handoff_handle || d != sdk_worker_handle ||
            (b & 3) || !mem::SubpageMap::allows(b, sizeof(uint32_t), 3)) stop_probe();
        const uint32_t owner = __atomic_load_n(reinterpret_cast<const uint32_t*>(b), __ATOMIC_SEQ_CST);
        if ((owner & 0x3fffffff) != a || !(owner & 0x40000000)) stop_probe();
        std::printf("SDK_WORKER_MUTEX owner=%llx address=%llx tag=%llx word=%x; stopped before waiting for parent\n",
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(d), owner);
        sdk_worker_wait = true; throw Handoff{};
    }
    constexpr bool allowed = startup_trace || Number == 1 || Number == 2 || Number == 3 || Number == 4 || Number == 5 || Number == 6 || Number == 8 || Number == 9 || Number == 0xc ||
        Number == 0xd || Number == 0xe || Number == 0xf || Number == 0x10 || Number == 0x16 ||
        Number == 0x1e || Number == 0x1f || Number == 0x21 || Number == 0x22 || Number == 0x24 ||
        Number == 0x25 || Number == 0x27 || Number == 0x29 || Number == 0x6f || Number == 0x7c;
#else
    constexpr bool allowed = Number == 2 || Number == 3 || Number == 6 || Number == 0x27 || Number == 0x29;
#endif
    if (!rtld_handlers[Number] || !allowed) {
        std::printf("RTLD_STOP svc=%02zx fn=%llx lr=%llx x0=%llx x1=%llx x2=%llx\n", Number,
            static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(d));
        stop_probe();
    }
    if constexpr (Number == 0x27) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(b, 240));
        if (n && mem::SubpageMap::allows(a, n, 1))
            std::printf("RTLD_DEBUG %.*s\n", static_cast<int>(n), reinterpret_cast<const char*>(a));
    }
#ifdef NXRT_TEST_SDK
    bool check_ipc_result = false, tipc_request = false;
    size_t cmif_header = 0;
    std::string ipc_target;
    uint32_t ipc_command = UINT32_MAX;
    nxrt::diagnostic::IpcArgumentSnapshot ipc_arguments;
    const uint64_t message_address = Number == 0x22 ? a : c->tpidrro;
    const uint64_t message_bytes = Number == 0x22 ? b : 0x100;
    if constexpr (Number == 0x21 || Number == 0x22) {
        const auto session = nxrt::kernel::handle_as<nxrt::ipc::KSession>(Number == 0x22 ? d : a);
        if (session && message_bytes >= 8 && message_bytes <= 0x1000 &&
            mem::SubpageMap::allows(message_address, message_bytes, 3)) {
            nxrt::hipc::Message message;
            if (nxrt::hipc::parse(reinterpret_cast<const uint32_t*>(message_address), message_bytes / 4, message)) {
                auto target = session->svc;
                size_t header = 0;
                uint32_t command = UINT32_MAX, magic = 0;
                const auto read_word = [&](size_t off) {
                    uint32_t value = UINT32_MAX;
                    if (off + 4 <= message.raw.size()) std::memcpy(&value, message.raw.data() + off, 4);
                    return value;
                };
                tipc_request = nxrt::hipc::is_tipc(message.type);
                if (tipc_request) command = message.type - nxrt::hipc::kTipcBase;
                else if (message.type == nxrt::hipc::kInvoke2MethodOld || message.type == nxrt::hipc::kInvoke2Method) {
                    if (session->domain) {
                        // IPC wrappers serialize domain inspection with guest requests.
                        if (!session->dom || message.raw.size() < 16) {
                            std::puts("SDK_STOP malformed domain header"); stop_probe();
                        }
                        if (message.raw[0] == 2) {
                            // Domain object close has no SFCI body. Let production
                            // IPC release it; the trace must not reject SDK teardown.
                            std::printf("SDK_DOMAIN_CLOSE object=%u\n", read_word(4));
                        } else {
                            if (message.raw[0] != 1 || message.raw.size() < 32) {
                                std::puts("SDK_STOP unsupported domain message"); stop_probe();
                            }
                            const auto object = session->dom->objects.find(read_word(4));
                            if (object == session->dom->objects.end()) {
                                std::puts("SDK_STOP unknown domain object"); stop_probe();
                            }
                            target = object->second;
                        }
                        header = 16;
                    }
                    magic = read_word(header);
                    if (magic == 0x49434653) command = read_word(header + 8);
                }
                if (target && command != UINT32_MAX) {
                    std::printf("SDK_REQUEST service=%s command=%u protocol=%s raw_bytes=%zu copies=%zu moves=%zu\n",
                        target->name().c_str(), command, tipc_request ? "TIPC" : "CMIF",
                        message.raw.size(), message.copy.size(), message.move.size());
                }
                if (target && (tipc_request || magic == 0x49434653)) {
                    check_ipc_result = true; cmif_header = header;
                    ipc_target = target->name(); ipc_command = command;
                    ipc_arguments = nxrt::diagnostic::snapshot_arguments(message, session->domain);
                }
            }
        }
    }
#endif
#ifdef NXRT_GAME_STARTUP_NATIVE
    if constexpr (Number == 0x01) {
        const auto started = std::chrono::steady_clock::now();
#ifdef NXRT_GAME_RENDER_NATIVE
        profile.begin_handler();
#endif
        rtld_handlers[Number](c);
#ifdef NXRT_GAME_RENDER_NATIVE
        profile.end_handler();
#endif
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("GAME_HEAP_RESULT requested=%llu rc=%llx base=%llx committed=%llu elapsed_ms=%lld\n",
            static_cast<unsigned long long>(b), static_cast<unsigned long long>(c->x[0]),
            static_cast<unsigned long long>(c->x[1]), static_cast<unsigned long long>(mem::heap_committed()),
            static_cast<long long>(elapsed));
        if (fsync(fileno(stdout)) || c->x[0]) {
            std::puts("GAME_STOP heap allocation failed or result checkpoint could not be persisted");
            stop_probe();
        }
    } else
#endif
    {
#ifdef NXRT_GAME_RENDER_NATIVE
        profile.begin_handler();
#endif
        rtld_handlers[Number](c);
#ifdef NXRT_GAME_RENDER_NATIVE
        profile.end_handler();
#endif
    }
    if constexpr (game_startup_trace) {
        if (nxrt::svc_unknown_total()) {
            std::printf("GAME_STOP unsupported system call=%02zx fn=%llx\n", Number, static_cast<unsigned long long>(c->fn));
            stop_probe();
        }
    }
#ifdef NXRT_TEST_SDK
    if constexpr (!startup_trace && Number == 0x08) {
        if (c->x[0] || !nxrt::kernel::handle_as<nxrt::kernel::KThread>(c->x[1])) stop_probe();
        sdk_worker_handle = c->x[1];
        sdk_thread_boundary = true;
    }
    if constexpr (!startup_trace && Number == 0x09) {
        if (c->x[0] || a != sdk_worker_handle || !nxrt::kernel::thread_join(sdk_worker_handle, 3000)) {
            std::puts("SDK_STOP worker failed to start or reach bounded wait"); std::fflush(stdout); std::abort();
        }
        const auto worker = nxrt::kernel::handle_as<nxrt::kernel::KThread>(sdk_worker_handle);
        if (!worker || !worker->worker || !sdk_worker_wait) stop_probe();
        const auto failure = worker->worker->failure();
        if (!failure) stop_probe();
        try { std::rethrow_exception(failure); } catch (const Handoff&) {
            std::puts("SDK_BOUNDARY worker executed SDK entry and requested its parent-owned mutex; joined after probe stop");
            throw Handoff{};
        }
    }
    if constexpr (!startup_trace && Number == 0x04) {
        if (c->x[0] || mem::SubpageMap::allows(b, d, 1) || !mem::SubpageMap::allows(a, d, 3) ||
            mem::vm_query(a).state != mem::kMemStack || !mem::stack_source_locked(b, d)) stop_probe();
        sdk_map_boundary = true;
        std::printf("SDK_MAP completed dst=%llx src=%llx size=%llx\n", static_cast<unsigned long long>(a),
            static_cast<unsigned long long>(b), static_cast<unsigned long long>(d));
    }
    if (check_ipc_result && c->x[0] == 0) {
        const auto reply = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(message_address),
            message_bytes / 4, tipc_request, cmif_header);
        if (nxrt::diagnostic::stop_on_reply(reply, ipc_target, ipc_command)) {
            std::printf("SDK_STOP service=%s command=%u protocol=%s result=%08x reply_magic=%08x malformed=%u\n",
                ipc_target.c_str(), ipc_command, tipc_request ? "TIPC" : "CMIF", reply.result, reply.magic,
                !reply.valid);
            std::printf("SDK_STOP_ARGS valid=%u exact_size=%u bytes=%zu captured=%zu hex=",
                ipc_arguments.valid, ipc_arguments.exact_size, ipc_arguments.bytes, ipc_arguments.captured);
            for (size_t n = 0; n < ipc_arguments.captured; ++n) std::printf("%02x", ipc_arguments.prefix[n]);
            std::printf(" fn=%llx lr=%llx\n", static_cast<unsigned long long>(c->fn),
                static_cast<unsigned long long>(c->x[30]));
            stop_probe();
        }
    }
    if constexpr (Number == 0x1f) {
        std::printf("SDK_PORT %s result=%llx handle=%llx\n", c->x[0] == 0 ? reinterpret_cast<const char*>(b) : "(rejected)",
            static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]));
    }
    if constexpr (Number == 0x21 || Number == 0x22) {
        if constexpr (!startup_trace) std::printf("SDK_IPC %s\n", nxrt::ipc::ipc_summary().c_str());
        for (const auto& stats : nxrt::ipc::service_stats()) {
            if (stats.requests && (stats.name == "IDisplayController" || stats.name == "IDebugFunctions")) {
                std::printf("SDK_STOP unbacked endpoint used: %s\n", stats.name.c_str()); stop_probe();
            }
            if (!stats.unhandled.empty()) {
                for (const auto& [cmd, count] : stats.unhandled)
                    std::printf("SDK_STOP service=%s command=%u calls=%llu\n", stats.name.c_str(), cmd, static_cast<unsigned long long>(count));
                stop_probe();
            }
        }
        const auto missing = nxrt::ipc::stub_names_requested();
        if (!missing.empty()) {
            for (const auto& name : missing) std::printf("SDK_STOP unported service=%s fn=%llx lr=%llx\n", name.c_str(),
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]));
            stop_probe();
        }
    }
#endif
#ifdef NXRT_GAME_RENDER_NATIVE
    const bool trace_result = trace_call || (Number != 0x1e && c->x[0]);
#else
    const bool trace_result = trace_call || c->x[0];
#endif
    if (trace_result) std::printf("RTLD_SVC n=%u svc=%02zx a=%llx b=%llx d=%llx rc=%llx out=%llx\n",
        call_number, Number, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
        static_cast<unsigned long long>(d), static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]));
}
template<size_t... Number> void install_rtld_svcs(std::index_sequence<Number...>) {
    ((rtld_handlers[Number] = nxrt::svc_handler(Number), nxrt::svc_register(Number, rtld_svc<Number>)), ...);
}
struct RtldCleanup {
    const nxrt::game::Info* previous = nxrt::game::current();
    bool installed = false;
    ~RtldCleanup() {
#ifdef NXRT_GAME_RENDER_NATIVE
        // This destructor runs only for failures before guest dispatch. The
        // running diagnostic exits its process without freeing live GPU views.
        nxrt::ps5_graphics::shutdown();
#endif
#ifdef NXRT_GAME_EH
        nxrt::hooks_clear(); nxrt::eh::set_typeinfo_vtables({});
        nxrt::eh::set_terminate_handler(nullptr); nxrt::eh::set_abi_autodetect();
#endif
        if (installed) for (unsigned n = 0; n < rtld_handlers.size(); ++n) nxrt::svc_register(n, rtld_handlers[n]);
        nxrt::kernel::main_thread_release(); nxrt::kernel::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::services::am_reset();
        nxrt::services::aoc_reset();
        nxrt::services::fs_reset();
        nxrt::services::time_reset();
        nxrt::svc_memory_reset_for_tests();
        nxrt::modules_unload_all(); nxrt::nxmodules_clear(); nxrt::hle_clear(); nxrt::hle_restore_current(nullptr);
        nxrt::svc_info_reset_for_tests(); nxrt::svc_memory_reset_for_tests(); mem::shutdown();
        nxrt::game::set_current(previous);
    }
};
struct RtldDeadline {
    std::mutex mutex; std::condition_variable changed; bool done = false;
    std::thread worker{[this] {
        std::unique_lock lock(mutex);
#ifdef NXRT_GAME_RENDER_NATIVE
        constexpr int seconds = 180;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        for (;;) {
            const auto next = std::min(end, std::chrono::steady_clock::now() + std::chrono::seconds(10));
            if (changed.wait_until(lock, next, [this] { return done; })) return;
            lock.unlock();
            if (std::chrono::steady_clock::now() >= end) {
                std::puts("RTLD_STOP watchdog expired");
                stop_probe();
            }
            report_startup_profile(false);
            lock.lock();
        }
#else
        constexpr int seconds = startup_trace ? 60 : 20;
        if (!changed.wait_for(lock, std::chrono::seconds(seconds), [this] { return done; })) {
            std::puts("RTLD_STOP watchdog expired"); std::fflush(stdout);
            if constexpr (startup_trace) stop_probe();
            std::abort();
        }
#endif
    }};
    ~RtldDeadline() { { std::lock_guard lock(mutex); done = true; } changed.notify_all(); worker.join(); }
};
}

#ifdef NXRT_GAME_STARTUP_TRACE
extern "C" [[noreturn]] void nxrt_game_startup_stop() { stop_probe(); }
#endif
#ifdef NXRT_TEST_SDK
#ifndef NXRT_GAME_EH
// Fail explicitly if startup reaches guest exception machinery before it is ported.
extern "C" void nx_catch_call(Ctx* c, void (*)(Ctx*, uint64_t)) {
    std::printf("SDK_STOP guest exception frame fn=%llx\n", static_cast<unsigned long long>(c->fn));
    stop_probe();
}
#endif
#ifdef NXRT_GAME_STARTUP_TRACE
NXTEST(decrypted_full_game_startup_trace_stops_without_reporting_a_pass) {
#else
NXTEST(decrypted_sdk_maps_its_stack_and_runs_a_worker_to_its_parent_mutex) {
#endif
#else
NXTEST(decrypted_rtld_reaches_the_sdk_handoff_without_executing_sdk_code) {
#endif
    std::string exefs = "/data/pokopia-recomp/game/exefs";
#ifdef NXRT_GAME_RENDER_NATIVE
    REQUIRE(nxrt::ps5_graphics::open_picker());
#endif
#ifdef NXRT_GAME_STARTUP_NATIVE
    std::string selected, location_error;
    const bool remembered = nxrt::app::load_picker_location("/data/pokopia-recomp/game-location.txt", selected, location_error);
    if (!remembered) std::printf("GAME_LOCATION_FAIL reason=%s\n", location_error.c_str());
    REQUIRE(remembered);
    const auto location = nxrt::app::check_game_location(selected, nxrt::app::picker_storage_roots());
    if (!location.usable) std::printf("GAME_LOCATION_FAIL reason=%s\n", location.message.c_str());
    REQUIRE(location.usable);
    // The current RomFS reader accepts one complete extracted tree. Refuse a
    // multi-layer selection until overlay resolution is implemented.
    if (location.game.romfs_layers.size() != 1) std::puts("GAME_LOCATION_FAIL multiple RomFS layers require overlay support");
    REQUIRE(location.game.romfs_layers.size() == 1);
    exefs = location.game.exefs;
    nxrt::config_set("fs", "romfs", location.game.romfs_layers.front().c_str());
    REQUIRE(nxrt::config_get_str("fs", "romfs") == location.game.romfs_layers.front());
    std::printf("GAME_LOCATION root=%s exefs=%s romfs=%s assets_copied=false\n", location.game.root.c_str(),
        exefs.c_str(), location.game.romfs_layers.front().c_str());
    {
#ifdef NXRT_GAME_RENDER_NATIVE
        REQUIRE(nxrt::ps5_graphics::startup_status("Reading game asset metadata from the selected drive."));
#endif
        nxrt::services::fs::RomfsImage assets;
        std::string error;
        const bool built = assets.build(location.game.romfs_layers.front(), &error);
        if (!built) std::printf("GAME_ROMFS_FAIL reason=%s\n", error.c_str());
        REQUIRE(built);
        const auto stats = assets.stats();
        REQUIRE(stats.files > 0);
        std::printf("GAME_ROMFS files=%u directories=%u data_bytes=%llu metadata_ready=true assets_copied=false\n",
            stats.files, stats.dirs, static_cast<unsigned long long>(stats.data_bytes));
    }
    REQUIRE(fsync(fileno(stdout)) == 0);
#endif
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
#ifdef NXRT_GAME_STARTUP_TRACE
    game.id = "pkp"; game.title = "Pokémon Pokopia";
    game.program_id = startup_program_id; game.version = startup_version;
    game.title_defaults = +[](nxrt::services::TitleProfile& profile) {
        profile.program_id = startup_program_id;
        profile.name = "Pokémon Pokopia";
    };
#endif
    RtldCleanup cleanup;
    nxrt::kernel::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::modules_unload_all(); nxrt::nxmodules_clear(); nxrt::hle_clear();
    nxrt::svc_memory_reset_for_tests(); nxrt::svc_info_reset_for_tests(); mem::shutdown();
    nxrt::game::set_current(&game);
#ifdef NXRT_GAME_RENDER_NATIVE
    // nninitStartup sizes its heap from the resource-limit queries. Advertise
    // the native port's budget through the existing shared accounting path;
    // do not truncate SetHeapSize or report successful backing we cannot supply.
    const auto native_budget = std::to_string(PKP_NATIVE_GAME_MEMORY_MIB);
    nxrt::config_set("memory", "app_memory_mb", native_budget.c_str());
    REQUIRE(nxrt::svc_total_memory() == (uint64_t{PKP_NATIVE_GAME_MEMORY_MIB} << 20));
    std::printf("GAME_MEMORY_PROFILE guest_application_mib=%u minimum_host_free_mib=1024\n",
        unsigned(PKP_NATIVE_GAME_MEMORY_MIB));
#endif
#ifdef NXRT_GAME_STARTUP_TRACE
    REQUIRE(nxrt::services::program_id_now() == startup_program_id);
#endif
#ifdef NXRT_TEST_SDK
    nxrt::services::logfault_reset(); nxrt::services::register_logfault(); nxrt::services::am_reset(); nxrt::services::register_am();
    nxrt::services::aoc_reset(); nxrt::services::register_aoc();
    nxrt::services::register_pctl();
    nxrt::services::fs_reset(); nxrt::services::register_fs();
    nxrt::services::time_reset(); nxrt::services::register_time();
#endif
    REQUIRE(mem::init({.heap_reserve = 12ull << 30}));
    nxrt::Module* rtld = nullptr;
    for (const auto& expected : game_image_inputs) {
        const auto path = exefs + "/" + expected.name;
#ifdef NXRT_GAME_RENDER_NATIVE
        REQUIRE(nxrt::ps5_graphics::startup_status(std::string("Checking and loading game module: ") + expected.name));
#endif
        REQUIRE(check_file(path, expected));
        nxrt::LoadOptions options; options.name = expected.name; options.base = expected.base;
        std::string error; auto* module = nxrt::module_load_nso_file(path.c_str(), options, &error);
        if (!module) std::printf("RTLD_LOAD failed: %s\n", error.c_str());
        REQUIRE(module);
        if (std::strcmp(expected.name, "rtld") == 0) rtld = module;
    }
#ifdef NXRT_TEST_SDK
#ifdef NXRT_GAME_STARTUP_TRACE
#ifdef NXRT_GAME_RENDER_NATIVE
    REQUIRE(nxrt::ps5_graphics::startup_status("Connecting game modules and native graphics."));
#endif
    REQUIRE(register_game_modules());
#else
    auto registered = nxrt::nxmodule_register(nxrt::module_by_name("sdk"), &nxmod_sdk, nxmod_sdk_abi);
    REQUIRE(registered.ok()); REQUIRE(registered.functions == static_cast<size_t>(nxmod_sdk.nfns));
#endif
    const auto sdk_entry = nxrt::nxmodule_find_export("_ZN2nn4init5StartEmmPFvvES2_S2_");
    sdk_start = nxrt::fn_lookup(sdk_entry); REQUIRE(sdk_start); nxrt::fn_register(sdk_entry, trace_sdk_start);
    if constexpr (!game_startup_trace) for (int n = 0; n < nxmod_sdk.nsyms; ++n) {
        const auto& symbol = nxmod_sdk.syms[n];
        if (symbol.defined || !symbol.name || !*symbol.name) continue;
        nxrt::hle_register(symbol.name, +[](::Ctx* c) {
            std::printf("SDK_STOP external call %s fn=%llx lr=%llx\n", c->in_import,
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]));
            stop_probe();
        });
    }
#endif
    REQUIRE(rtld); REQUIRE(nxrt::module_protect_all());
#ifdef NXRT_GAME_RENDER_NATIVE
    REQUIRE(nxrt::ps5_graphics::install_game_bridge());
#endif
#ifndef NXRT_GAME_STARTUP_TRACE
    REQUIRE(nxrt::module_register_nxmodule(rtld, &nxmod_rtld) == static_cast<size_t>(nxmod_rtld.nfns));
    unsigned imports = 0, start_imports = 0;
    for (int n = 0; n < nxmod_rtld.nsyms; ++n) {
        const auto& symbol = nxmod_rtld.syms[n];
        if (symbol.defined || !symbol.name || !*symbol.name) continue;
        ++imports; std::printf("RTLD_BOUNDARY import=%s\n", symbol.name);
        if (std::strcmp(symbol.name, "_ZN2nn4init5StartEmmPFvvES2_S2_") != 0) {
            nxrt::hle_register(symbol.name, +[](::Ctx*) {
                std::puts("RTLD_STOP unexpected external call before SDK startup"); stop_probe();
            });
            continue;
        }
        ++start_imports;
        nxrt::hle_register(symbol.name, +[](::Ctx* c) {
            rtld_handoff_handle = c->x[0];
            std::printf("RTLD_HANDOFF handle=%llx callbacks=%llx,%llx,%llx; SDK code not executed\n",
                static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[2]),
                static_cast<unsigned long long>(c->x[3]), static_cast<unsigned long long>(c->x[4]));
            throw Handoff{};
        });
    }
    REQUIRE(imports == 2 && start_imports == 1);
#else
    // Let the supplied rtld apply relocations. Host linking would patch its
    // input first and would no longer exercise the native loader path.
    for (const auto& expected : game_image_inputs) {
        REQUIRE(!nxrt::module_by_name(expected.name)->linked());
        constexpr uint32_t permissions[] = {5, 1, 3};
        for (unsigned n = 0; n < 3; ++n)
            REQUIRE(mem::vm_query(expected.base + expected.segments[n].offset).perm == permissions[n]);
    }
#endif
    nxrt::svc_init(); nxrt::svc_reset_counters(); rtld_calls = 0; rtld_handoff_handle = 0; sdk_map_boundary = false; sdk_thread_boundary = false; sdk_stack_address = 0; sdk_worker_handle = 0; sdk_worker_wait = false;
    install_rtld_svcs(std::make_index_sequence<nxrt::kSvcCount>{}); cleanup.installed = true;
    const auto main_handle = nxrt::kernel::main_thread_adopt(0x100000, "rtld-probe"); REQUIRE(main_handle);
    auto* context = nxrt::kernel::current_ctx(); REQUIRE(context);
    context->x[0] = 0; context->x[1] = main_handle; context->x[30] = 0;
    bool handoff = false;
    {
#ifdef NXRT_GAME_RENDER_NATIVE
        REQUIRE(nxrt::ps5_graphics::startup_status("Starting game services. Waiting for the first game frame."));
        startup_started_ns = startup_now_ns();
#endif
        RtldDeadline deadline;
        try { nxrt::dispatch_call(context, rtld->base()); }
        catch (const Handoff&) { handoff = true; }
        catch (const ProbeStop&) { std::puts("BOOT_STOP before expected diagnostic boundary"); }
    }
    if constexpr (startup_trace) { std::puts("SDK_TRACE returned from SDK entry"); stop_probe(); }
    CHECK(handoff);
#ifdef NXRT_TEST_SDK
    CHECK(sdk_map_boundary); CHECK(sdk_thread_boundary); CHECK(sdk_worker_wait); CHECK_EQ(rtld_handoff_handle, main_handle);
    CHECK_EQ(sdk_callback_returns[0], 1u); CHECK_EQ(sdk_callback_returns[1], 0u); CHECK_EQ(sdk_callback_returns[2], 0u);
    CHECK(nxrt::ipc::stub_names_requested().empty());
    bool saw_sm = false, saw_logger = false, saw_applet = false, saw_proxy = false, saw_window = false;
    bool saw_common = false, saw_self = false, saw_application = false;
    for (const auto& stats : nxrt::ipc::service_stats()) {
        CHECK(!stats.stub); CHECK(stats.unhandled.empty());
        if (stats.name == "sm:" && stats.requests >= 3) saw_sm = true;
        if (stats.name == "lm" && stats.requests >= 1) saw_logger = true;
        if (stats.name == "appletOE" && stats.requests == 1) saw_applet = true;
        if (stats.name == "IApplicationProxy" && stats.requests == 10) saw_proxy = true;
        if (stats.name == "IWindowController" && stats.requests == 1) saw_window = true;
        if (stats.name == "ICommonStateGetter" && stats.requests == 1) saw_common = true;
        if (stats.name == "ISelfController" && stats.requests == 1) saw_self = true;
        if (stats.name == "IApplicationFunctions" && stats.requests == 1) saw_application = true;
    }
    CHECK(saw_sm); CHECK(saw_logger); CHECK(saw_applet); CHECK(saw_proxy); CHECK(saw_window);
    CHECK(saw_common); CHECK(saw_self); CHECK(saw_application);
#else
    CHECK_EQ(rtld_handoff_handle, main_handle);
#endif
    CHECK(rtld_calls > 0); CHECK_EQ(nxrt::svc_unknown_total(), 0u);
    std::printf("SDK_RESULT map_completed=%u worker_created=%u worker_wait=%u\n", sdk_map_boundary, sdk_thread_boundary, sdk_worker_wait);
    std::printf("RTLD_RESULT handoff=%u svc_calls=%u lifted_functions=%d; no gameplay executed\n", handoff, rtld_calls.load(), nxmod_rtld.nfns);
}
#endif
