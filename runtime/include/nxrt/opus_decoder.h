// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace nxrt::audio {
struct OpusConfig {
    int rate = 48000, channels = 2, streams = 1, coupled = 1;
    bool multistream = false, large_frame = false;
    std::array<unsigned char, 256> mapping{};
};
enum class OpusResult { Ok, InvalidArgument, InvalidPacket, BufferTooSmall, InvalidRange };
struct OpusDecoded { OpusResult result; uint32_t consumed = 0, samples = 0; uint64_t microseconds = 0; };
// Owns one serialized decoder context. Callers serialize each instance.
// Packet input includes the eight-byte big-endian size/final-range header.
class OpusStream {
public:
    static bool valid(const OpusConfig&, bool check_mapping = true);
    static uint32_t work_size(const OpusConfig&);
    static std::unique_ptr<OpusStream> create(const OpusConfig&);
    OpusDecoded decode(std::span<const uint8_t> packet, std::span<int16_t> pcm, bool reset);
    const OpusConfig& config() const { return config_; }
private:
    OpusConfig config_;
    std::vector<uint8_t> state_, previous_;
    std::vector<int16_t> samples_;
};
}
