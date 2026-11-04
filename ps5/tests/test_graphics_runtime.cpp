// SPDX-License-Identifier: GPL-3.0-or-later
// Real runtime graphics integration: VMA uploads, shader pipelines, sampled
// Tegra texture/mip data and Renderer capture/presentation. No game commands.
#include "nxrt/gfx.h"
#include "nxrt/scene.h"
#include "nxrt/platform.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/ps5_vm.h"
#include "../app/folder_picker.h"
#include "../gfx/picker_draw.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

extern "C" int sceSystemServiceHideSplashScreen();

namespace {
bool verify_first_frame(const std::vector<uint8_t>& image, uint32_t width, uint32_t height) {
    if (width < 64 || height < 64 || image.size() != static_cast<size_t>(width) * height * 4) return false;
    const auto texture = nxrt::gfx::TestScene::make_texture_levels(nullptr);
    struct Point { float u, v; bool small; };
    const Point samples[] = {{0.05f, 0.05f, false}, {0.29f, 0.47f, false}, {0.7f, 0.4f, false},
                             {0.77f, 0.81f, false}, {0.27f, 0.43f, true}, {0.69f, 0.72f, true}};
    bool ok = true;
    for (const auto& sample : samples) {
        const float half = sample.small ? 0.16f : 0.62f;
        const float center_x = sample.small ? 0.78f : 0.0f;
        const float center_y = sample.small ? -0.72f : 0.0f;
        const int x = static_cast<int>((center_x + 1.0f) * width * 0.5f + (sample.u * 2 - 1) * half * height * 0.5f);
        const int y = static_cast<int>((center_y + 1.0f) * height * 0.5f + (sample.v * 2 - 1) * half * height * 0.5f);
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return false;
        const float u = (((2.0f * (x + 0.5f) / width - 1) - center_x) * width / height / half + 1) * 0.5f;
        const float v = (((2.0f * (y + 0.5f) / height - 1) - center_y) / half + 1) * 0.5f;
        const int side = sample.small ? 64 : 256;
        const size_t offset = sample.small ? (256 * 256 + 128 * 128) * 4 : 0;
        const float tx = u * side - 0.5f, ty = v * side - 0.5f;
        const int ix = static_cast<int>(std::floor(tx)), iy = static_cast<int>(std::floor(ty));
        const float fx = tx - ix, fy = ty - iy;
        unsigned worst = 0;
        for (int channel = 0; channel < 3; ++channel) {
            auto texel = [&](int px, int py) {
                return texture[offset + (std::clamp(py, 0, side - 1) * side + std::clamp(px, 0, side - 1)) * 4 + channel];
            };
            const float a = texel(ix, iy) * (1 - fx) + texel(ix + 1, iy) * fx;
            const float b = texel(ix, iy + 1) * (1 - fx) + texel(ix + 1, iy + 1) * fx;
            const int expected = static_cast<int>(std::lround(a * (1 - fy) + b * fy));
            const int actual = image[(static_cast<size_t>(y) * width + x) * 4 + channel];
            worst = std::max(worst, static_cast<unsigned>(std::abs(actual - expected)));
        }
        std::printf("RUNTIME_SCENE_PIXEL x=%d y=%d mip=%d error=%u tolerance=3\n", x, y, sample.small ? 2 : 0, worst);
        ok &= worst <= 3;
    }
    const size_t at = (static_cast<size_t>(10) * width + 10) * 4;
    const uint8_t expected[] = {13, 20, 36, 255};
    for (size_t c = 0; c < 4; ++c) ok &= std::abs(static_cast<int>(image[at + c]) - expected[c]) <= 1;
    std::printf("RUNTIME_SCENE_READBACK status=%s bytes=%zu width=%u height=%u samples=7\n", ok ? "PASS" : "FAIL", image.size(), width, height);
    return ok;
}
}

extern "C" int pkp_graphics_runtime_probe() {
    using namespace nxrt;
    std::puts("RUNTIME_SCENE_BEGIN device=real renderer=real scene=shader-texture game_code=false NVN_replay=false");
    LogOptions logging;
    logging.path = "";
    logging.stderr_all = true;
    log_init(logging);
    // Retain the production guest windows while the real renderer allocates.
    auto heap = ps5_vm::Reservation::reserve(mem::kHeapMax, mem::kHeapBase);
    auto alias = ps5_vm::Reservation::reserve(mem::kAliasSize, mem::kAliasBase);
    auto stack = ps5_vm::Reservation::reserve(mem::kStackSize, mem::kStackBase);
    auto kernel = ps5_vm::Reservation::reserve(mem::kKernelSize, mem::kKernelBase);
    if (!heap || !alias || !stack || !kernel) { std::puts("RUNTIME_SCENE_FAIL operation=guest-reservations"); return 1; }
    extras::Settings settings;
    settings.hdr.mode = extras::HdrMode::Off;
    settings.upscale.mode = extras::UpscaleMode::Off;
    settings.upscale.frame_generation = false;
    settings.latency.mode = extras::LatencyMode::Off;
    extras::set_settings(settings);
    if (!platform::init({false})) return 1;
    platform::WindowDesc window;
    window.title = "Pokopia runtime renderer test";
    window.resizable = false;
    if (!platform::create_window(window)) { platform::shutdown(); return 1; }
    std::string error;
    gfx::DeviceDesc descriptor;
    descriptor.app_name = "Pokopia PS5 runtime renderer";
    descriptor.headless = 0;
    descriptor.validation = 0;
    auto device = gfx::Device::create(descriptor, &error);
    std::unique_ptr<gfx::Renderer> renderer;
    std::unique_ptr<gfx::TestScene> scene;
    bool ok = device != nullptr;
    if (ok) {
        gfx::RendererDesc desc;
        desc.present_mode = gfx::PresentMode::Fifo;
        desc.frames_in_flight = 2;
        renderer = gfx::Renderer::create(*device, desc, &error);
        ok = renderer != nullptr;
    }
    bool cancelled = false;
    if (ok) {
        app::FolderPicker picker;
        std::printf("RUNTIME_PICKER_SPLASH result=%08x\n", static_cast<unsigned>(sceSystemServiceHideSplashScreen()));
        while (ok && !picker.view().done && !picker.view().cancelled) {
            picker.update();
            gfx::FrameInfo frame;
            if (!renderer->begin_frame(&frame)) { error = "picker begin_frame failed"; ok = false; break; }
            app::draw_folder_picker(frame, picker.view());
            renderer->end_frame();
            if (log_count(LogLevel::Error) != 0) { error = "native renderer reported an error during folder selection"; ok = false; }
        }
        cancelled = picker.view().cancelled;
        if (picker.view().done) std::printf("RUNTIME_PICKER_SELECTED path=%s assets_copied=false\n", picker.path().c_str());
    }
    const auto before_scene = renderer ? renderer->stats() : gfx::RendererStats{};
    if (ok && !cancelled) { scene = gfx::TestScene::create(*device, renderer->format(), &error); ok = scene != nullptr; }
    std::vector<uint8_t> first, last;
    uint32_t width = 0, height = 0, last_width = 0, last_height = 0, completed = 0;
    if (ok && !cancelled) {
        ok = scene->tiling_round_trip_ok() && scene->tiled_bytes() > 0;
        std::printf("RUNTIME_SCENE_READY tiling=%s tiled_bytes=%llu surface=%ux%u\n", ok ? "PASS" : "FAIL",
            static_cast<unsigned long long>(scene->tiled_bytes()), renderer->extent().width, renderer->extent().height);
        std::printf("RUNTIME_SCENE_SPLASH result=%08x\n", static_cast<unsigned>(sceSystemServiceHideSplashScreen()));
    }
    for (; ok && !cancelled && completed < 300; ++completed) {
        if (completed == 0) renderer->capture_next_frame(&first, &width, &height);
        if (completed == 299) renderer->capture_next_frame(&last, &last_width, &last_height);
        gfx::FrameInfo frame;
        if (!renderer->begin_frame(&frame)) { error = "begin_frame failed"; ok = false; break; }
        frame.frame_number = completed; // Stable scene oracle despite time spent in the picker.
        scene->record(frame);
        renderer->end_frame();
        if (completed == 0) ok = verify_first_frame(first, width, height);
    }
    if (ok && !cancelled) {
        const auto stats = renderer->stats();
        ok = first.size() == last.size() && first != last && width == last_width && height == last_height &&
            stats.presents-before_scene.presents == 300 && stats.frames-before_scene.frames == 300 && log_count(LogLevel::Error) == 0;
        std::printf("RUNTIME_SCENE_ANIMATION changed=%s frames=%llu presents=%llu errors=%llu\n", first != last ? "yes" : "no",
            static_cast<unsigned long long>(stats.frames-before_scene.frames), static_cast<unsigned long long>(stats.presents-before_scene.presents),
            static_cast<unsigned long long>(log_count(LogLevel::Error)));
    }
    if (device) device->wait_idle();
    scene.reset(); renderer.reset(); device.reset();
    platform::shutdown();
    ok &= log_count(LogLevel::Error) == 0 && !platform::has_window() && !platform::initialised();
    std::printf("RUNTIME_SCENE_RESULT status=%s frames=%u/300 error=%s gameplay_verified=false\n", ok ? (cancelled ? "CANCELLED" : "PASS") : "FAIL", completed, error.c_str());
    log_shutdown();
    return ok ? 0 : 1;
}
