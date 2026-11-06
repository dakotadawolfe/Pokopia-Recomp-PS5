// SPDX-License-Identifier: GPL-3.0-or-later
// Adapt the upstream Switch 2 graphics handoff to the actual PS5 NVN renderer.
#include "game_renderer.h"
#include "sdk_graphics_decode.h"
#include "offline_guest_displays.h"
#include "picker_draw.h"
#include "../app/folder_picker.h"
#include "../../runtime/src/gfx/nvn/nvn_shaders.h"
#include "../../runtime/src/services/fs_romfs.h"
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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <cerrno>
#include <ctime>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <ps5platform/heap.h>

extern "C" int sceSystemServiceHideSplashScreen();
extern "C" int64_t sceKernelGetDirectMemorySize();
extern "C" int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
extern "C" [[noreturn]] void nxrt_game_startup_stop();

namespace nxrt::ps5_graphics {
namespace {
std::unique_ptr<gfx::Device> device;
std::unique_ptr<gfx::Renderer> renderer;
std::mutex mutex;
uint64_t flag_address = 0, pointer_address = 0, driver = 0, display = 0, layer = 0, window = 0;
std::atomic<bool> initialized{false};
bool vi_initialized = false;
OfflineGuestDisplays guest_displays;
std::atomic<uint64_t> presents{0};
std::string selected_folder;
std::chrono::steady_clock::time_point loading_started;
std::atomic<uint64_t> loading_views{0};
bool consume_request(const char* path) {
    FILE* request = std::fopen(path, "rb");
    if (!request) return false;
    char token[32]{};
    const auto length = std::fread(token, 1, sizeof token, request);
    const bool valid = !std::ferror(request) && length == 10 && std::memcmp(token, "PPSA98732\n", 10) == 0;
    std::fclose(request);
    return unlink(path) == 0 && valid;
}
// Three bounded, title-owned readbacks distinguish an intentionally black
// guest frame from an image lost between the NVN target and the display.
std::vector<uint8_t> captured_frame;
uint32_t capture_width = 0, capture_height = 0;
uint64_t capture_number = 0;
unsigned requested_captures = 0;
void capture_frame(uint64_t number) {
    if (capture_number && !renderer->capture_pending()) {
        const uint64_t bytes = uint64_t(capture_width) * capture_height * 4;
        if (bytes && bytes <= (32u << 20) && captured_frame.size() == bytes) {
            uint64_t colored = 0;
            uint8_t maximum = 0;
            for (size_t i = 0; i < captured_frame.size(); i += 4) {
                const auto value = std::max({captured_frame[i], captured_frame[i+1], captured_frame[i+2]});
                maximum = std::max(maximum, value);
                colored += value != 0;
            }
            char path[200];
            const unsigned slot = capture_number == 3 ? 0 : capture_number == 120 ? 1 : capture_number == 600 ? 2 : 3;
            std::snprintf(path, sizeof path, "/download0/pokopia-frame-capture/capture-%u.pam", slot);
            bool saved = false;
            if (mkdir("/download0/pokopia-frame-capture", 0755) == 0 || errno == EEXIST) {
                if (FILE* out = std::fopen(path, "wb")) {
                    // Keep four files below 25 MiB in total, leaving the title's
                    // small download volume available for its save journal.
                    const uint32_t step = std::max((capture_width + 1919) / 1920, (capture_height + 1079) / 1080);
                    const uint32_t width = (capture_width + step - 1) / step;
                    const uint32_t height = (capture_height + step - 1) / step;
                    saved = std::fprintf(out, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n",
                        width, height) > 0;
                    std::vector<uint8_t> row(size_t(width) * 3);
                    for (uint32_t y = 0; saved && y < height; ++y) {
                        for (uint32_t x = 0; x < width; ++x) {
                            const auto source = (size_t(y * step) * capture_width + x * step) * 4;
                            std::memcpy(row.data() + size_t(x) * 3, captured_frame.data() + source, 3);
                        }
                        saved = std::fwrite(row.data(), 1, row.size(), out) == row.size();
                    }
                    saved = std::fclose(out) == 0 && saved;
                    if (!saved) unlink(path);
                }
            }
            std::printf("GAME_FRAME_PIXELS frame=%llu width=%u height=%u colored_pixels=%llu max_rgb=%u saved=%d path=%s\n",
                static_cast<unsigned long long>(capture_number), capture_width, capture_height,
                static_cast<unsigned long long>(colored), unsigned(maximum), saved, saved ? path : "(none)");
        } else {
            std::printf("GAME_FRAME_PIXELS frame=%llu readback_unavailable=true\n", static_cast<unsigned long long>(capture_number));
        }
        captured_frame.clear();
        capture_number = 0;
    }
    bool requested = false;
    if (number % 60 == 0 && requested_captures < 3 && !renderer->capture_pending()) {
        constexpr const char* request_path = "/data/pokopia-recomp/tests/pokopia-capture-once";
        requested = consume_request(request_path);
        if (requested) ++requested_captures;
    }
    if ((number == 3 || number == 120 || number == 600 || requested) && !renderer->capture_pending()) {
        capture_number = number;
        capture_width = capture_height = 0;
        renderer->capture_next_frame(&captured_frame, &capture_width, &capture_height);
    }
}
bool loading_idle() {
    // The SDK's initial blank presentation is not a ready menu. Stop the shell
    // view permanently when the app declares itself running or presents again.
    if (services::amctl::startup_state().notified_running || presents.load() >= 2) return false;
    if (!initialized.load()) return true;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-loading_started).count();
    const auto io = services::fs::romfs_io_stats();
    app::PickerView view;
    view.title = "STARTING POKOPIA";
    view.location = selected_folder;
    view.status = "Preparing game: " + std::to_string(seconds) + " seconds. Assets read: " + std::to_string(io.bytes/(1024*1024)) + " MB.";
    view.controls = "WAITING FOR GAME STARTUP";
    view.controller_connected = true;
    gfx::FrameInfo fi;
    if (!renderer->begin_frame(&fi)) return false;
    app::draw_folder_picker(fi, view); renderer->end_frame();
    loading_views.fetch_add(1, std::memory_order_relaxed);
    return true;
}
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
    guest_displays.destroy();
    layer = window = 0;
}
void native_window(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !layer || c->x[1] != layer) fail("invalid native-window request");
    write<uint64_t>(c->x[0], window); c->x[0] = 0;
}
void create_guest_displays(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !initialized || !layer ||
        !guest_displays.create(static_cast<int32_t>(c->x[0]), window))
        fail("invalid offline guest-display creation");
    std::printf("GAME_GUEST_DISPLAYS count=%u backend=PS5-NVN streaming=false\n", static_cast<uint32_t>(c->x[0]));
    c->x[0] = 0;
}
void destroy_guest_displays(Ctx* c) {
    std::lock_guard lock(mutex);
    guest_displays.destroy(); c->x[0] = 0;
}
void guest_native_window(Ctx* c) {
    std::lock_guard lock(mutex);
    const auto result = guest_displays.window(static_cast<uint32_t>(c->x[0]));
    if (!ready() || !layer || !result || result != window) fail("invalid offline guest-window request");
    c->x[0] = result;
}
void guest_luminance(Ctx* c) {
    std::lock_guard lock(mutex);
    if (!ready() || !layer || !guest_displays.window(static_cast<uint32_t>(c->x[0])))
        fail("invalid offline guest-display metadata");
    // No remote display is connected. Guest HDR metadata must not change the
    // primary display's presentation settings.
    c->x[0] = 0;
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
    capture_frame(n);
    if (n <= 3 || n % 60 == 0) std::printf("GAME_NVN_PRESENT n=%llu\n", static_cast<unsigned long long>(n));
}
}

void prepare_capture_storage() {
    if (!consume_request("/data/pokopia-recomp/tests/pokopia-clear-captures")) return;
    const char* directory = "/download0/pokopia-frame-capture";
    DIR* entries = opendir(directory);
    if (!entries) {
        std::printf("GAME_CAPTURE_CLEANUP open_failed=true errno=%d\n", errno);
        return;
    }
    unsigned removed = 0, failed = 0, inspected = 0, matched = 0;
    while (auto* entry = readdir(entries)) {
        ++inspected;
        unsigned long long stamp = 0, frame = 0;
        unsigned pid = 0;
        int length = 0;
        if (std::sscanf(entry->d_name, "%llu-%u-%llu.pam%n", &stamp, &pid, &frame, &length) != 3 ||
            length != int(std::strlen(entry->d_name)) || !stamp || !pid ||
            (frame != 3 && frame != 120 && frame != 600)) continue;
        ++matched;
        char path[512];
        const int written = std::snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        if (written < 0 || size_t(written) >= sizeof path) { ++failed; continue; }
        // The native app sandbox denies lstat. Like the folder picker, inspect
        // an opened descriptor without following links instead.
        const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) { ++failed; continue; }
        struct stat status{};
        const int metadata_result = fstat(fd, &status);
        const int close_result = close(fd);
        if (metadata_result || close_result || !S_ISREG(status.st_mode)) { ++failed; continue; }
        if (unlink(path) == 0) ++removed;
        else ++failed;
    }
    closedir(entries);
    std::printf("GAME_CAPTURE_CLEANUP removed=%u failed=%u inspected=%u matched=%u\n", removed, failed, inspected, matched);
}

bool open_picker() {
    const auto errors_before = log_count(LogLevel::Error);
    config_set("nvn", "pipeline_cache", "/data/pokopia-recomp/graphics-cache/pipelines.bin");
    config_set("nvn", "bc7_cache", "/data/pokopia-recomp/graphics-cache/bc7");
    // Private, title-contained capture of shaders absent from the static index.
    // The first-frame diagnostic has a fixed lifetime and a bounded byte budget.
    config_set("nvn", "dump_missing", "/download0/pokopia-shader-capture");
    config_set("nvn", "dump_missing_max_bytes", "16777216");
    config_set("nvn", "trace_frame", "0,1,2,3,4,5");
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
    app::PickerOptions picker_options;
    // A host-controlled, one-use request for the already selected folder. The
    // request is consumed before validation, so a crash cannot create a loop.
    constexpr const char* run_once = "/data/pokopia-recomp/tests/pokopia-run-once";
    if (FILE* request = std::fopen(run_once, "rb")) {
        char token[32]{};
        const auto length = std::fread(token, 1, sizeof token, request);
        const bool read_ok = !std::ferror(request);
        std::fclose(request);
        const bool consumed = unlink(run_once) == 0;
        constexpr char expected[] = "PPSA98732\n";
        picker_options.use_saved = read_ok && consumed && length == sizeof expected - 1 &&
            std::memcmp(token, expected, sizeof expected - 1) == 0;
        std::printf("GAME_AUTORUN consumed=%d valid=%d\n", consumed, picker_options.use_saved);
    }
    app::FolderPicker picker(std::move(picker_options));
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
        {"_ZN2nn10streamplay24CreateIndividualDisplaysEi", create_guest_displays},
        {"_ZN2nn10streamplay25DestroyIndividualDisplaysEv", destroy_guest_displays},
        {"_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", guest_native_window},
        {"_ZN2nn10streamplay52SetLayerNextHdrStandardContentLuminanceRangeMetadataENS0_7GuestIdEtt", guest_luminance},
        {"_ZN2nn10streamplay52SetLayerNextHdrExtendedContentLuminanceRangeMetadataENS0_7GuestIdEtt", guest_luminance},
    };
    // Preflight every export before mutating the import routing.
    for (const auto& f : functions)
        if (!sdk->find_symbol(f.name) || !fn_lookup(sdk->find_symbol(f.name))) return bridge_error(f.name);
    const auto bootstrap = sdk->find_symbol("nvnBootstrapLoader");
    if (!bootstrap || !fn_lookup(bootstrap)) return bridge_error("NVN bootstrap export is not registered");
    nvn::BackendDesc bd; bd.device = device.get(); bd.renderer = renderer.get(); bd.keep_aspect = true;
    loading_started = std::chrono::steady_clock::now(); bd.idle = loading_idle;
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
    std::printf("GAME_LOADING_VIEWS count=%llu guest_frames=false\n", static_cast<unsigned long long>(loading_views.load()));
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
        const auto words = nvn::be::shader_word_stats();
        struct ps5_heap_stats heap{};
        ps5_heap_stats(&heap);
        int64_t free_at = 0;
        size_t largest_free = 0;
        const int available = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(),
            16384, &free_at, &largest_free);
        std::printf("GAME_MEMORY_PROGRESS shader_words=%zu shader_word_entries=%zu word_reads=%llu word_hits=%llu "
                    "host_heap_mapped=%zu host_heap_peak=%zu host_heap_segments=%u libc_fallbacks=%llu "
                    "largest_direct_free=%zu direct_query_rc=%08x\n",
            words.resident_bytes, words.entries, static_cast<unsigned long long>(words.reads),
            static_cast<unsigned long long>(words.hits), heap.mapped_bytes, heap.peak_bytes,
            heap.segments, heap.libc_fallbacks, largest_free, static_cast<unsigned>(available));
    }
}
void shutdown() {
    diag_set_frame_listener(nullptr);
    guest_displays.destroy();
    nvn::shutdown_backend(); renderer.reset(); device.reset(); platform::shutdown();
    flag_address = pointer_address = driver = display = layer = window = 0;
    initialized = vi_initialized = false;
}
}
