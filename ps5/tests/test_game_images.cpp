// SPDX-License-Identifier: GPL-3.0-or-later
// Checks supplied images and optionally runs their rtld up to the SDK boundary.
#include "test_framework.h"
#include "private_game_images.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/dispatch.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#ifdef NXRT_TEST_RTLD
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services.h"
#include "nxrt/subpage.h"
#include "nxrt/svc.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
namespace nxrt { void svc_memory_reset_for_tests(); void svc_info_reset_for_tests(); }
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
struct Handoff {};
struct ProbeStop {};
std::array<nxrt::SvcHandler, nxrt::kSvcCount> rtld_handlers{};
unsigned rtld_calls = 0;
uint64_t rtld_handoff_handle = 0;
template<size_t Number> void rtld_svc(::Ctx* c) {
    if (++rtld_calls > 10000) { std::puts("RTLD_STOP system-call limit"); throw ProbeStop{}; }
    const uint64_t a = c->x[0], b = c->x[1], d = c->x[2];
    if (!rtld_handlers[Number] || (Number != 2 && Number != 3 && Number != 6 && Number != 0x27 && Number != 0x29)) {
        std::printf("RTLD_STOP svc=%02zx fn=%llx lr=%llx x0=%llx x1=%llx x2=%llx\n", Number,
            static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(c->x[30]),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(d));
        throw ProbeStop{};
    }
    if constexpr (Number == 0x27) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(b, 240));
        if (n && mem::SubpageMap::allows(a, n, 1))
            std::printf("RTLD_DEBUG %.*s\n", static_cast<int>(n), reinterpret_cast<const char*>(a));
    }
    rtld_handlers[Number](c);
    if (rtld_calls <= 80 || c->x[0]) std::printf("RTLD_SVC n=%u svc=%02zx a=%llx b=%llx d=%llx rc=%llx out=%llx\n",
        rtld_calls, Number, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
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
        nxrt::kernel::main_thread_release(); nxrt::kernel::kernel_reset();
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
            std::puts("RTLD_STOP watchdog expired"); std::fflush(stdout); std::abort();
        }
    }};
    ~RtldDeadline() { { std::lock_guard lock(mutex); done = true; } changed.notify_all(); worker.join(); }
};
}

NXTEST(decrypted_rtld_reaches_the_sdk_handoff_without_executing_sdk_code) {
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    RtldCleanup cleanup;
    nxrt::kernel::kernel_reset(); nxrt::modules_unload_all(); nxrt::nxmodules_clear(); nxrt::hle_clear();
    nxrt::svc_memory_reset_for_tests(); nxrt::svc_info_reset_for_tests(); mem::shutdown();
    nxrt::game::set_current(&game);
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
    REQUIRE(rtld); REQUIRE(nxrt::module_protect_all());
    REQUIRE(nxrt::module_register_nxmodule(rtld, &nxmod_rtld) == static_cast<size_t>(nxmod_rtld.nfns));
    unsigned imports = 0, start_imports = 0;
    for (int n = 0; n < nxmod_rtld.nsyms; ++n) {
        const auto& symbol = nxmod_rtld.syms[n];
        if (symbol.defined || !symbol.name || !*symbol.name) continue;
        ++imports; std::printf("RTLD_BOUNDARY import=%s\n", symbol.name);
        if (std::strcmp(symbol.name, "_ZN2nn4init5StartEmmPFvvES2_S2_") != 0) {
            nxrt::hle_register(symbol.name, +[](::Ctx*) {
                std::puts("RTLD_STOP unexpected external call before SDK startup"); throw ProbeStop{};
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
    nxrt::svc_init(); nxrt::svc_reset_counters(); rtld_calls = 0; rtld_handoff_handle = 0;
    install_rtld_svcs(std::make_index_sequence<nxrt::kSvcCount>{}); cleanup.installed = true;
    const auto main_handle = nxrt::kernel::main_thread_adopt(0x100000, "rtld-probe"); REQUIRE(main_handle);
    auto* context = nxrt::kernel::current_ctx(); REQUIRE(context);
    context->x[0] = 0; context->x[1] = main_handle; context->x[30] = 0;
    bool handoff = false;
    {
        RtldDeadline deadline;
        try { nxrt::dispatch_call(context, rtld->base()); }
        catch (const Handoff&) { handoff = true; }
        catch (const ProbeStop&) { std::puts("RTLD_STOP before SDK handoff"); }
    }
    CHECK(handoff); CHECK_EQ(rtld_handoff_handle, main_handle);
    CHECK(rtld_calls > 0); CHECK_EQ(nxrt::svc_unknown_total(), 0u);
    std::printf("RTLD_RESULT handoff=%u svc_calls=%u lifted_functions=%d; no gameplay executed\n", handoff, rtld_calls, nxmod_rtld.nfns);
}
#endif
