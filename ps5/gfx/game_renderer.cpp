// SPDX-License-Identifier: GPL-3.0-or-later
// Adapt the upstream Switch 2 graphics handoff to the actual PS5 NVN renderer.
#include "game_renderer.h"
#include "sdk_graphics_decode.h"
#include "picker_draw.h"
#include "../app/folder_picker.h"
#include "../../runtime/src/gfx/nvn/nvn_shaders.h"
#include "nxrt/diag.h"
#include "nxrt/am_ps5.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/nxmodule.h"
#include "nxrt/platform.h"
#include "nxrt/subpage.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

extern "C" int sceSystemServiceHideSplashScreen();
extern "C" [[noreturn]] void nxrt_game_startup_stop();

namespace nxrt::ps5_graphics {
namespace {
std::unique_ptr<gfx::Device> device;
std::unique_ptr<gfx::Renderer> renderer;
std::mutex mutex;
uint64_t flag_address = 0, pointer_address = 0, driver = 0, display = 0, layer = 0, window = 0;
std::atomic<bool> initialized{false};
bool vi_initialized = false;
std::atomic<uint64_t> presents{0};
std::string selected_folder;
[[noreturn]] void fail(const char* why) {
    std::printf("GAME_GRAPHICS_STOP reason=%s\n", why); nxrt_game_startup_stop();
}
bool writable(uint64_t address, size_t bytes) {
    return address && mem::is_mapped(address, bytes) && mem::SubpageMap::allows(address, bytes, 3);
}
template<class T> void write(uint64_t address, T value) {
    if (!writable(address, sizeof(T))) fail("invalid writable SDK output");
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
uint64_t token(uint64_t kind) {
    const auto address = mem::alloc_kernel(256, 16);
    if (!address) fail("host graphics object allocation failed");
    write<uint64_t>(address, kind);
    return address;
}
bool ready() { return renderer && platform::has_window() && nvn::backend_installed(); }
bool bridge_error(const char* why) {
    std::printf("GAME_GRAPHICS_BRIDGE_FAIL reason=%s\n", why);
    return false;
}
void initialize_graphics(Ctx*) {
    std::lock_guard lock(mutex);
    if (!ready() || !flag_address || !pointer_address || !driver) fail("native NVN backend is unavailable");
    uint64_t current = 0;
    if (!mem::copy_readable(pointer_address, &current, sizeof current) || (current && current != driver))
        fail("SDK driver pointer already belongs to another implementation");
    // This is an opaque identity for our real host backend. Console driver code
    // is never invoked through it; all supported NVN/display calls route here.
    write<uint64_t>(pointer_address, driver);
    write<uint32_t>(flag_address, 1);
    initialized = true;
    std::printf("GAME_GRAPHICS_READY backend=PS5-NVN flag=%llx driver_slot=%llx\n",
        static_cast<unsigned long long>(flag_address), static_cast<unsigned long long>(pointer_address));
}
void vi_initialize(Ctx*) {
    std::lock_guard lock(mutex);
    if (!ready() || !initialized) fail("display initialization precedes native graphics");
    vi_initialized = true;
}
void vi_finalize(Ctx*) {
    std::lock_guard lock(mutex);
    if (display || layer) fail("display finalized with live objects");
    vi_initialized = false;
}
void open_display(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !vi_initialized || display) fail("invalid default-display lifecycle");
    display = token(0x505335444953504cull);
    write<uint64_t>(c->x[0], display); c->x[0] = 0;
}
void close_display(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!display || c->x[0] != display || layer) fail("invalid display close");
    display = 0;
}
void create_layer(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !display || c->x[1] != display || layer) fail("invalid layer creation");
    layer = token(0x5053354c41594552ull); window = token(0x50533557494e444full);
    write<uint64_t>(c->x[0], layer); c->x[0] = 0;
}
void destroy_layer(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!layer || c->x[0] != layer) fail("invalid layer destruction");
    layer = window = 0;
}
void native_window(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !layer || c->x[1] != layer) fail("invalid native-window request");
    write<uint64_t>(c->x[0], window); c->x[0] = 0;
}
void vrr_capability(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !display || c->x[0] != display) fail("invalid VRR display");
    // The title declares its capability; the host retains its fixed FIFO mode.
    std::printf("GAME_DISPLAY capability_vrr=%llu host_mode=fifo\n", static_cast<unsigned long long>(c->x[1]));
    c->x[0] = 0;
}
template<bool Extended> void luminance(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !layer || c->x[0] != layer) fail("invalid layer metadata");
    extras::hdr_note_layer_luminance(Extended, static_cast<uint16_t>(c->x[1]), static_cast<uint16_t>(c->x[2]));
    c->x[0] = 0;
}
void frame() {
    int width = 0, height = 0; platform::pixel_size(&width, &height);
    services::amctl::set_display_resolution(width, height);
    const auto n = presents.fetch_add(1) + 1;
    if (n <= 3 || n % 60 == 0) std::printf("GAME_NVN_PRESENT n=%llu\n", static_cast<unsigned long long>(n));
}
}

bool open_picker() {
    const auto errors_before = log_count(LogLevel::Error);
    config_set("nvn", "pipeline_cache", "/data/pokopia-recomp/graphics-cache/pipelines.bin");
    config_set("nvn", "bc7_cache", "/data/pokopia-recomp/graphics-cache/bc7");
    extras::Settings settings;
    settings.hdr.mode = extras::HdrMode::Off; settings.upscale.mode = extras::UpscaleMode::Off;
    settings.upscale.frame_generation = false; settings.latency.mode = extras::LatencyMode::Off;
    extras::set_settings(settings);
    if (!platform::init({false}) || !platform::create_window({.title="Pokopia native startup", .resizable=false})) return false;
    std::string error;
    gfx::DeviceDesc dd; dd.app_name = "Pokopia native startup"; dd.headless = 0; dd.validation = 0;
    device = gfx::Device::create(dd, &error);
    if (device) renderer = gfx::Renderer::create(*device, {.present_mode=gfx::PresentMode::Fifo, .frames_in_flight=2}, &error);
    if (!renderer) { std::printf("GAME_RENDERER_FAIL error=%s\n", error.c_str()); return false; }
    app::FolderPicker picker;
    (void)sceSystemServiceHideSplashScreen();
    while (!picker.view().done && !picker.view().cancelled) {
        picker.update(); gfx::FrameInfo fi;
        if (!renderer->begin_frame(&fi)) return false;
        app::draw_folder_picker(fi, picker.view()); renderer->end_frame();
    }
    if (!picker.view().done || log_count(LogLevel::Error) != errors_before) return false;
    selected_folder = picker.path();
    return startup_status("Checking the selected game files.");
}
bool startup_status(std::string_view message) {
    if (!renderer || initialized.load()) return false;
    app::PickerView view;
    view.title = "STARTING POKOPIA";
    view.location = selected_folder;
    view.status = std::string(message);
    view.controls = "GAME FILES REMAIN ON YOUR SELECTED DRIVE";
    view.controller_connected = true;
    gfx::FrameInfo fi;
    if (!renderer->begin_frame(&fi)) return false;
    app::draw_folder_picker(fi, view);
    renderer->end_frame();
    std::printf("GAME_LOADING stage=%.*s\n", static_cast<int>(message.size()), message.data());
    return true;
}
bool install_game_bridge() {
    const auto* sdk = module_by_name("sdk");
    if (!sdk || !renderer || nvn::backend_installed()) return bridge_error("invalid backend installation order");
    int width = 0, height = 0; platform::pixel_size(&width, &height);
    if (!services::amctl::set_display_resolution(width, height)) return bridge_error("native display size is unavailable");
    std::printf("GAME_DISPLAY width=%d height=%d\n", width, height);
    const auto check = sdk->find_symbol("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm");
    const auto driver_check = sdk->find_symbol("_ZN2nv9InitCheckEv");
    std::array<uint32_t, 4> flag_code{}; std::array<uint32_t, 24> driver_code{};
    if (!sdk->contains(check) || !sdk->contains(driver_check) ||
        !mem::copy_readable(check, flag_code.data(), sizeof flag_code) ||
        !mem::copy_readable(driver_check, driver_code.data(), sizeof driver_code))
        return bridge_error("SDK graphics checks are missing or unreadable");
    flag_address = initialized_flag(check, flag_code); pointer_address = driver_pointer(driver_check, driver_code);
    if (!sdk->contains(flag_address) || !sdk->contains(flag_address + 3) ||
        !sdk->contains(pointer_address) || !sdk->contains(pointer_address + 7) ||
        flag_address % 4 || pointer_address % 8 || !writable(flag_address, 4) || !writable(pointer_address, 8)) {
        std::printf("GAME_GRAPHICS_SLOTS flag=%llx pointer=%llx\n",
            static_cast<unsigned long long>(flag_address), static_cast<unsigned long long>(pointer_address));
        return bridge_error("SDK graphics slot decoding or write validation failed");
    }
    const struct { const char* name; HleFn function; } functions[] = {
        {"_ZN2nv18InitializeGraphicsEPvm", initialize_graphics},
        {"_ZN2nv18InitializeGraphicsEv", initialize_graphics},
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi8FinalizeEv", vi_finalize},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", open_display},
        {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", close_display},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", create_layer},
        {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", destroy_layer},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", native_window},
        {"_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", vrr_capability},
        {"_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", luminance<true>},
        {"_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", luminance<false>},
    };
    // Preflight every export before mutating the import routing.
    for (const auto& f : functions)
        if (!sdk->find_symbol(f.name) || !fn_lookup(sdk->find_symbol(f.name))) return bridge_error(f.name);
    const auto bootstrap = sdk->find_symbol("nvnBootstrapLoader");
    if (!bootstrap || !fn_lookup(bootstrap)) return bridge_error("NVN bootstrap export is not registered");
    nvn::BackendDesc bd; bd.device = device.get(); bd.renderer = renderer.get(); bd.keep_aspect = true;
    if (!nvn::install_backend(bd)) return bridge_error("native NVN backend failed to initialize");
    driver = token(0x5053354e564e4354ull);
    for (const auto& f : functions) {
        hle_register(f.name, f.function); module_override_symbol(f.name);
        fn_register(sdk->find_symbol(f.name), f.function);
    }
    module_override_symbol("nvnBootstrapLoader"); fn_register(bootstrap, hle_lookup("nvnBootstrapLoader"));
    diag_set_frame_listener(frame);
    std::printf("GAME_GRAPHICS_BRIDGE exports=%zu bootstrap=%llx sdk_flags_validated=true\n",
        std::size(functions)+1, static_cast<unsigned long long>(bootstrap));
    return true;
}
void report() {
    const auto startup = services::amctl::startup_state();
    std::printf("GAME_APPLET_STATE loading_mode=%u notified_running=%d host_clock_policy=fixed\n",
        startup.cpu_boost_mode, startup.notified_running);
    const auto s = nvn::backend_stats();
    std::printf("GAME_RENDER_REPORT backend=%d initialized=%d submits=%llu draws=%llu presents=%llu\n",
        nvn::backend_installed(), initialized.load(), static_cast<unsigned long long>(s.submits),
        static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.presents));
    if (initialized.load()) {
        auto& cache = nvn::be::shader_cache();
        const auto timing = cache.lookup_stats();
        std::printf("GAME_SHADER_PROGRESS loaded=%zu missing=%zu lookups=%llu lookup_ms=%llu max_lookup_ms=%llu\n",
            cache.loaded(), cache.missing(), static_cast<unsigned long long>(timing.calls),
            static_cast<unsigned long long>(timing.total_ns / 1000000),
            static_cast<unsigned long long>(timing.max_ns / 1000000));
    }
}
void shutdown() {
    diag_set_frame_listener(nullptr);
    nvn::shutdown_backend(); renderer.reset(); device.reset(); platform::shutdown();
    flag_address = pointer_address = driver = display = layer = window = 0;
    initialized = vi_initialized = false;
}
}
