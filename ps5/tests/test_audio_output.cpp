// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "../../runtime/src/services/audren/Server/AudioRendererManager.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace nxrt::audio { std::shared_ptr<AudioSink> make_ps5_sink(); }
NXTEST_GROUP(audio_output)
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<unsigned> outputs{0}, closes{0};
std::mutex capture_mutex;
std::array<int16_t, 512> captured{};
}
#ifndef NXRT_TARGET_PS5
extern "C" int sceAudioOutInit() { return 0; }
extern "C" int sceAudioOutOpen(int, int, int, uint32_t frames, uint32_t rate, uint32_t format) {
    return frames == 256 && rate == 48000 && format == 1 ? 7 : -1;
}
extern "C" int sceAudioOutSetVolume(int, int, const int*) { return 0; }
extern "C" int sceAudioOutOutput(int port, const void* data) {
    if (port != 7) return -1;
    if (data) {
        { std::lock_guard lock(capture_mutex); std::copy_n(static_cast<const int16_t*>(data), captured.size(), captured.begin()); }
        std::this_thread::sleep_for(std::chrono::microseconds(5333));
        ++outputs;
    }
    return 0;
}
extern "C" int sceAudioOutClose(int) { ++closes; return 0; }
#endif

NXTEST(audio_device_drains_and_can_be_reopened) {
    auto sink = nxrt::audio::make_ps5_sink();
    REQUIRE(sink);
    CHECK(!sink->open(44100, 2)); CHECK(!sink->open(48000, 3));
    for (unsigned pass = 0; pass < 3; ++pass) {
        REQUIRE(sink->open(48000, 2));
        REQUIRE(sink->open(48000, 2));
        std::array<int16_t, 4096 * 2> pcm{}; // Longer than the device's initial queue; remains silent.
        const auto start = Clock::now();
        CHECK_EQ(sink->write(pcm.data(), 4096), 4096u);
        const auto deadline = start + std::chrono::seconds(2);
        while (sink->queued_frames() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK_EQ(sink->queued_frames(), 0u);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        std::printf("AudioOut pass=%u frames=4096 elapsed_us=%lld\n", pass, static_cast<long long>(elapsed));
        CHECK(elapsed >= 40000);
        sink->close();
    }
    CHECK_EQ(sink->stats().frames_accepted, 12288u);
    CHECK_EQ(sink->stats().frames_dropped, 0u);
}

#ifndef NXRT_TARGET_PS5
NXTEST(audio_device_downmix_and_volume_preserve_pcm) {
    auto sink = nxrt::audio::make_ps5_sink();
    REQUIRE(sink->open(48000, 6));
    nxrt::audio::set_master_volume(0.5f);
    std::array<int16_t, 256 * 6> pcm; pcm.fill(1000);
    CHECK_EQ(sink->write(pcm.data(), 256), 256u);
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (sink->queued_frames() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sink->close();
    nxrt::audio::set_master_volume(1.0f);
    std::lock_guard lock(capture_mutex);
    CHECK_EQ(captured[0], 1103); CHECK_EQ(captured[1], 1103);
}
#endif

NXTEST(audio_tick_matches_registered_console_frequency) {
    nxrt::game::Info game{}; game.platform = nxrt::game::Platform::Switch2;
    const auto* previous = nxrt::game::current();
    nxrt::game::set_current(&game);
    nxrt::audren::TickSource tick;
    const auto start = Clock::now(); const auto first = tick.Counter();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const double measured = double(tick.Counter() - first) / tick.Frequency();
    CHECK(measured > seconds * 0.8 && measured < seconds * 1.2);
    nxrt::game::set_current(previous);
}
