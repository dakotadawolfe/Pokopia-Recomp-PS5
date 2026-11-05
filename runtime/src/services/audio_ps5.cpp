// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/audio_sink.h"
#include "nxrt/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(NXRT_TARGET_PS5) || defined(NXRT_AUDIO_TEST_DEVICE)
// Public payload SDK AudioOut ABI; 256-frame, 48 kHz stereo PCM16 output.
extern "C" int sceAudioOutInit();
extern "C" int sceAudioOutOpen(int, int, int, uint32_t, uint32_t, uint32_t);
extern "C" int sceAudioOutSetVolume(int, int, const int*);
extern "C" int sceAudioOutOutput(int, const void*);
extern "C" int sceAudioOutClose(int);

namespace nxrt::audio {
namespace {
class Ps5Sink final : public AudioSink {
    static constexpr size_t grain = 256, capacity = 8192;
    std::mutex lifecycle_, mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::array<std::array<int16_t, 2>, capacity> ring_{};
    size_t head_ = 0, size_ = 0, in_flight_ = 0;
    uint32_t channels_ = 0;
    int port_ = -1;
    bool stopping_ = true, failed_ = false;
    SinkStats stats_{};

    void stop_locked() {
        { std::lock_guard lock(mutex_); stopping_ = true; wake_.notify_all(); }
        if (thread_.joinable()) thread_.join();
        if (port_ >= 0) {
            sceAudioOutOutput(port_, nullptr);
            const int rc = sceAudioOutClose(port_);
            NXLOG_I("audio", "PS5 output closed: rc=%08x", static_cast<unsigned>(rc));
            port_ = -1;
        }
        std::lock_guard lock(mutex_);
        head_ = size_ = in_flight_ = 0;
    }
    void consume() {
        std::array<int16_t, grain * 2> block{};
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || size_ != 0; });
            // The guest produces 240-frame updates; AudioOut consumes 256. Coalesce
            // adjacent updates instead of adding silence to every guest frame.
            wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopping_ || size_ >= grain; });
            if (stopping_) break;
            block.fill(0);
            const size_t count = std::min(size_, grain);
            if (count < grain) ++stats_.underruns;
            for (size_t i = 0; i < count; ++i) {
                block[i * 2] = ring_[head_][0]; block[i * 2 + 1] = ring_[head_][1];
                head_ = (head_ + 1) % capacity;
            }
            size_ -= count; in_flight_ = count;
            lock.unlock();
            const int rc = sceAudioOutOutput(port_, block.data());
            lock.lock();
            in_flight_ = 0;
            if (rc < 0) {
                failed_ = true; stats_.frames_dropped += size_ + count; size_ = 0;
                lock.unlock();
                NXLOG_E("audio", "PS5 output failed: rc=%08x", static_cast<unsigned>(rc));
                return;
            }
        }
    }
public:
    ~Ps5Sink() override { close(); }
    bool open(uint32_t rate, uint32_t channels) override {
        std::lock_guard life(lifecycle_);
        if (rate != 48000 || (channels != 2 && channels != 6)) return false;
        { std::lock_guard lock(mutex_); if (port_ >= 0 && !failed_ && !stopping_ && channels == channels_) return true; }
        stop_locked();
        static std::once_flag once;
        static int init = -1;
        NXLOG_I("audio", "PS5 output initializing");
        std::call_once(once, [] { init = sceAudioOutInit(); });
        if (init < 0 && init != static_cast<int>(0x8026000e)) {
            NXLOG_E("audio", "PS5 output initialization failed: rc=%08x", static_cast<unsigned>(init)); return false;
        }
        NXLOG_I("audio", "PS5 output opening: init_rc=%08x", static_cast<unsigned>(init));
        port_ = sceAudioOutOpen(0xff, 0, 0, grain, 48000, 1);
        if (port_ < 0) {
            NXLOG_E("audio", "PS5 output open failed: rc=%08x", static_cast<unsigned>(port_)); return false;
        }
        const std::array<int, 8> volume = {0x8000, 0x8000};
        NXLOG_I("audio", "PS5 output setting volume: port=%d", port_);
        const int volume_rc = sceAudioOutSetVolume(port_, 3, volume.data());
        if (volume_rc < 0) {
            NXLOG_E("audio", "PS5 output volume failed: rc=%08x", static_cast<unsigned>(volume_rc));
            stop_locked(); return false;
        }
        { std::lock_guard lock(mutex_); channels_ = channels; stopping_ = failed_ = false; stats_.sample_rate = rate; }
        try { thread_ = std::thread([this] { consume(); }); }
        catch (...) { stop_locked(); throw; }
        NXLOG_I("audio", "PS5 output opened: 48000 Hz, %u input channels, 256-frame stereo blocks", channels);
        return true;
    }
    void close() override { std::lock_guard life(lifecycle_); stop_locked(); }
    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        const float volume = effective_volume();
        std::lock_guard lock(mutex_);
        stats_.frames_written += frames;
        if (!pcm || stopping_ || failed_) { stats_.frames_dropped += frames; return 0; }
        const size_t take = std::min<size_t>(frames, capacity - size_);
        auto sample = [volume](float x) { return static_cast<int16_t>(std::clamp(x * volume, -32768.0f, 32767.0f)); };
        for (size_t i = 0; i < take; ++i) {
            const int16_t* in = pcm + i * channels_;
            float left = in[0], right = in[1];
            if (channels_ == 6) { left += 0.7071f * in[2] + 0.5f * in[4]; right += 0.7071f * in[2] + 0.5f * in[5]; }
            ring_[(head_ + size_ + i) % capacity] = {sample(left), sample(right)};
        }
        size_ += take;
        stats_.frames_accepted += take; stats_.frames_dropped += frames - take;
        stats_.queued_peak_frames = std::max(stats_.queued_peak_frames, static_cast<uint32_t>(size_ + in_flight_));
        wake_.notify_one();
        return static_cast<uint32_t>(take);
    }
    uint32_t queued_frames() override { std::lock_guard lock(mutex_); return static_cast<uint32_t>(size_ + in_flight_); }
    bool has_device_clock() const override { return true; }
    const char* name() const override { return "ps5"; }
    SinkStats stats() override { std::lock_guard lock(mutex_); return stats_; }
};
}
std::shared_ptr<AudioSink> make_ps5_sink() { return std::make_shared<Ps5Sink>(); }
}
#else
namespace nxrt::audio {
std::shared_ptr<AudioSink> make_ps5_sink() { return make_paced_sink(); }
}
#endif
