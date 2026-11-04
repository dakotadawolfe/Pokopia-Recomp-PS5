// SPDX-License-Identifier: GPL-3.0-or-later
// Checks supplied images and optionally runs their rtld up to the SDK boundary.
#include "test_framework.h"
#include "private_game_images.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/dispatch.h"
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
uint32_t crc(const uint8_t* data, size_t size) { return crc_update(~0u, data, size) ^ ~0u; }
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

#ifdef NXRT_TEST_RTLD
namespace {
extern "C" const NxModule nxmod_rtld;
#ifdef NXRT_TEST_SDK
extern "C" const NxModule nxmod_sdk;
extern "C" const uint32_t nxmod_sdk_abi;
#endif
struct Handoff {};
struct ProbeStop {};
#ifdef NXRT_SDK_STARTUP_TRACE
constexpr bool startup_trace = true;
#else
constexpr bool startup_trace = false;
#endif
[[noreturn]] void stop_probe() {
    if constexpr (startup_trace) {
        std::puts("SDK_TRACE_END stopped at dependency or diagnostic limit; not a passing test or gameplay result");
        std::fflush(stdout); std::fflush(stderr);
        std::_Exit(2); // Ends this diagnostic process and all of its guest workers.
    }
    throw ProbeStop{};
}
std::mutex sdk_ipc_probe_mutex;
std::array<nxrt::SvcHandler, nxrt::kSvcCount> rtld_handlers{};
std::atomic<unsigned> rtld_calls{0};
uint64_t rtld_handoff_handle = 0;
bool sdk_map_boundary = false, sdk_thread_boundary = false;
uint64_t sdk_stack_address = 0;
uint32_t sdk_worker_handle = 0;
bool sdk_worker_wait = false;
#ifdef NXRT_TEST_SDK
nxrt::GuestFn sdk_start = nullptr;
std::array<nxrt::GuestFn, 3> sdk_callbacks{};
std::array<unsigned, 3> sdk_callback_returns{};
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
    if (call_number > (startup_trace ? 1000u : 10000u)) { std::puts("RTLD_STOP system-call limit"); stop_probe(); }
    const uint64_t a = c->x[0], b = c->x[1], d = c->x[2];
    if constexpr (startup_trace) {
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
    bool check_cmif_result = false;
    size_t cmif_header = 0;
    std::string ipc_target;
    uint32_t ipc_command = UINT32_MAX;
    if constexpr (Number == 0x21) {
        const auto session = nxrt::kernel::handle_as<nxrt::ipc::KSession>(a);
        if (session && mem::SubpageMap::allows(c->tpidrro, 0x100, 3)) {
            nxrt::hipc::Message message;
            if (nxrt::hipc::parse(reinterpret_cast<const uint32_t*>(c->tpidrro), 64, message)) {
                auto target = session->svc;
                size_t header = 0;
                uint32_t command = UINT32_MAX, magic = 0;
                const auto read_word = [&](size_t off) {
                    uint32_t value = UINT32_MAX;
                    if (off + 4 <= message.raw.size()) std::memcpy(&value, message.raw.data() + off, 4);
                    return value;
                };
                if (nxrt::hipc::is_tipc(message.type)) command = message.type - nxrt::hipc::kTipcBase;
                else if (message.type == nxrt::hipc::kInvoke2MethodOld || message.type == nxrt::hipc::kInvoke2Method) {
                    if (session->domain) {
                        // IPC wrappers serialize domain inspection with guest requests.
                        if (!session->dom || message.raw.size() < 32 || message.raw[0] != 1) stop_probe();
                        const auto object = session->dom->objects.find(read_word(4));
                        if (object == session->dom->objects.end()) stop_probe();
                        target = object->second; header = 16;
                    }
                    magic = read_word(header);
                    if (magic == 0x49434653) command = read_word(header + 8);
                }
                if (target && command != UINT32_MAX)
                    std::printf("SDK_REQUEST service=%s command=%u\n", target->name().c_str(), command);
                if (target && magic == 0x49434653) {
                    check_cmif_result = true; cmif_header = header;
                    ipc_target = target->name(); ipc_command = command;
                }
            }
        }
    }
#endif
    rtld_handlers[Number](c);
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
    if (check_cmif_result && c->x[0] == 0) {
        nxrt::hipc::Message reply;
        if (!nxrt::hipc::parse(reinterpret_cast<const uint32_t*>(c->tpidrro), 64, reply) ||
            reply.raw.size() < cmif_header + 12) stop_probe();
        uint32_t magic = 0, result = 0;
        std::memcpy(&magic, reply.raw.data() + cmif_header, 4);
        std::memcpy(&result, reply.raw.data() + cmif_header + 8, 4);
        if (magic != 0x4f434653 || result) {
            std::printf("SDK_STOP service=%s command=%u result=%08x reply_magic=%08x\n",
                ipc_target.c_str(), ipc_command, result, magic); stop_probe();
        }
    }
    if constexpr (Number == 0x1f) {
        std::printf("SDK_PORT %s result=%llx handle=%llx\n", c->x[0] == 0 ? reinterpret_cast<const char*>(b) : "(rejected)",
            static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]));
    }
    if constexpr (Number == 0x21 || Number == 0x22) {
        std::printf("SDK_IPC %s\n", nxrt::ipc::ipc_summary().c_str());
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
    if (startup_trace || call_number <= 100 || c->x[0]) std::printf("RTLD_SVC n=%u svc=%02zx a=%llx b=%llx d=%llx rc=%llx out=%llx\n",
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
        if (installed) for (unsigned n = 0; n < rtld_handlers.size(); ++n) nxrt::svc_register(n, rtld_handlers[n]);
        nxrt::kernel::main_thread_release(); nxrt::kernel::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::services::am_reset();
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
        if (!changed.wait_for(lock, std::chrono::seconds(20), [this] { return done; })) {
            std::puts("RTLD_STOP watchdog expired"); std::fflush(stdout);
            if constexpr (startup_trace) stop_probe();
            std::abort();
        }
    }};
    ~RtldDeadline() { { std::lock_guard lock(mutex); done = true; } changed.notify_all(); worker.join(); }
};
}

#ifdef NXRT_TEST_SDK
// Fail explicitly if startup reaches guest exception machinery before it is ported.
extern "C" void nx_catch_call(Ctx* c, void (*)(Ctx*, uint64_t)) {
    std::printf("SDK_STOP guest exception frame fn=%llx\n", static_cast<unsigned long long>(c->fn));
    stop_probe();
}
NXTEST(decrypted_sdk_maps_its_stack_and_runs_a_worker_to_its_parent_mutex) {
#else
NXTEST(decrypted_rtld_reaches_the_sdk_handoff_without_executing_sdk_code) {
#endif
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    RtldCleanup cleanup;
    nxrt::kernel::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::modules_unload_all(); nxrt::nxmodules_clear(); nxrt::hle_clear();
    nxrt::svc_memory_reset_for_tests(); nxrt::svc_info_reset_for_tests(); mem::shutdown();
    nxrt::game::set_current(&game);
#ifdef NXRT_TEST_SDK
    nxrt::services::logfault_reset(); nxrt::services::register_logfault(); nxrt::services::am_reset(); nxrt::services::register_am();
#endif
    REQUIRE(mem::init({.heap_reserve = 12ull << 30}));
    nxrt::Module* rtld = nullptr;
    for (const auto& expected : game_image_inputs) {
        const auto path = std::string("/data/pokopia-recomp/game/exefs/") + expected.name;
        REQUIRE(check_file(path, expected));
        nxrt::LoadOptions options; options.name = expected.name; options.base = expected.base;
        std::string error; auto* module = nxrt::module_load_nso_file(path.c_str(), options, &error);
        if (!module) std::printf("RTLD_LOAD failed: %s\n", error.c_str());
        REQUIRE(module);
        if (std::strcmp(expected.name, "rtld") == 0) rtld = module;
    }
#ifdef NXRT_TEST_SDK
    auto registered = nxrt::nxmodule_register(nxrt::module_by_name("sdk"), &nxmod_sdk, nxmod_sdk_abi);
    REQUIRE(registered.ok()); REQUIRE(registered.functions == static_cast<size_t>(nxmod_sdk.nfns));
    const auto sdk_entry = nxrt::nxmodule_find_export("_ZN2nn4init5StartEmmPFvvES2_S2_");
    sdk_start = nxrt::fn_lookup(sdk_entry); REQUIRE(sdk_start); nxrt::fn_register(sdk_entry, trace_sdk_start);
    for (int n = 0; n < nxmod_sdk.nsyms; ++n) {
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
    nxrt::svc_init(); nxrt::svc_reset_counters(); rtld_calls = 0; rtld_handoff_handle = 0; sdk_map_boundary = false; sdk_thread_boundary = false; sdk_stack_address = 0; sdk_worker_handle = 0; sdk_worker_wait = false;
    install_rtld_svcs(std::make_index_sequence<nxrt::kSvcCount>{}); cleanup.installed = true;
    const auto main_handle = nxrt::kernel::main_thread_adopt(0x100000, "rtld-probe"); REQUIRE(main_handle);
    auto* context = nxrt::kernel::current_ctx(); REQUIRE(context);
    context->x[0] = 0; context->x[1] = main_handle; context->x[30] = 0;
    bool handoff = false;
    {
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
