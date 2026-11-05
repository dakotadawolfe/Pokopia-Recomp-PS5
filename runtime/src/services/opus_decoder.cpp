// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/opus_decoder.h"
#include <opus.h>
#include <opus_multistream.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace nxrt::audio {
namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
int state_size(const OpusConfig& c) {
    return c.multistream ? opus_multistream_decoder_get_size(c.streams, c.coupled) : opus_decoder_get_size(c.channels);
}
int capacity(const OpusConfig& c) { return c.rate * (c.large_frame ? 120 : 40) / 1000; }
}
bool OpusStream::valid(const OpusConfig& c, bool check_mapping) {
    if (c.rate != 8000 && c.rate != 12000 && c.rate != 16000 && c.rate != 24000 && c.rate != 48000) return false;
    if (!c.multistream) return c.channels == 1 || c.channels == 2;
    if (c.channels < 1 || c.channels > 255 || c.streams < 1 || c.streams > 255 ||
        c.coupled < 0 || c.coupled > c.streams || c.streams + c.coupled > 255) return false;
    if (check_mapping) for (int n = 0; n < c.channels; ++n)
        if (c.mapping[n] != 255 && c.mapping[n] >= c.streams + c.coupled) return false;
    return true;
}
uint32_t OpusStream::work_size(const OpusConfig& c) {
    if (!valid(c, false)) return 0;
    const int size = state_size(c);
    if (size <= 0) return 0;
    // Host state, rollback snapshot and bounded PCM scratch; page-aligned for
    // the client's transfer-memory allocation. No arbitrary hardware size.
    const uint64_t bytes = uint64_t(size) * 2 + uint64_t(capacity(c)) * c.channels * sizeof(int16_t);
    return static_cast<uint32_t>((bytes + 0xfff) & ~uint64_t(0xfff));
}
std::unique_ptr<OpusStream> OpusStream::create(const OpusConfig& c) {
    if (!valid(c)) return {};
    auto stream = std::make_unique<OpusStream>(); stream->config_ = c;
    stream->state_.resize(state_size(c)); stream->previous_.resize(stream->state_.size());
    stream->samples_.resize(size_t(capacity(c)) * c.channels);
    const int result = c.multistream ? opus_multistream_decoder_init(
        reinterpret_cast<OpusMSDecoder*>(stream->state_.data()), c.rate, c.channels, c.streams, c.coupled, c.mapping.data()) :
        opus_decoder_init(reinterpret_cast<OpusDecoder*>(stream->state_.data()), c.rate, c.channels);
    return result == OPUS_OK ? std::move(stream) : nullptr;
}
OpusDecoded OpusStream::decode(std::span<const uint8_t> packet, std::span<int16_t> pcm, bool reset) {
    if (packet.size() < 9) return {OpusResult::InvalidPacket};
    const uint32_t bytes = be32(packet.data()), expected_range = be32(packet.data() + 4);
    if (!bytes || bytes > packet.size() - 8 || bytes > uint32_t(std::numeric_limits<int32_t>::max())) return {OpusResult::InvalidPacket};
    const auto* input = packet.data() + 8;
    const int expected = opus_packet_get_nb_samples(input, bytes, config_.rate);
    if (expected <= 0) return {OpusResult::InvalidPacket};
    if (expected > capacity(config_) || size_t(expected) * config_.channels > pcm.size()) return {OpusResult::BufferTooSmall};
    std::memcpy(previous_.data(), state_.data(), state_.size());
    auto* single = reinterpret_cast<OpusDecoder*>(state_.data());
    auto* multi = reinterpret_cast<OpusMSDecoder*>(state_.data());
    const auto began = std::chrono::steady_clock::now();
    if (reset) {
        const int rc = config_.multistream ? opus_multistream_decoder_ctl(multi, OPUS_RESET_STATE) : opus_decoder_ctl(single, OPUS_RESET_STATE);
        if (rc != OPUS_OK) {
            std::memcpy(state_.data(), previous_.data(), state_.size());
            return {OpusResult::InvalidArgument};
        }
    }
    const int decoded = config_.multistream ? opus_multistream_decode(multi, input, bytes, samples_.data(), capacity(config_), 0) :
        opus_decode(single, input, bytes, samples_.data(), capacity(config_), 0);
    opus_uint32 actual_range = 0;
    const int range_rc = config_.multistream ? opus_multistream_decoder_ctl(multi, OPUS_GET_FINAL_RANGE(&actual_range)) :
        opus_decoder_ctl(single, OPUS_GET_FINAL_RANGE(&actual_range));
    if (decoded <= 0 || decoded != expected || range_rc != OPUS_OK || (expected_range && actual_range != expected_range)) {
        std::memcpy(state_.data(), previous_.data(), state_.size());
        return {decoded > 0 && range_rc == OPUS_OK && expected_range && actual_range != expected_range ?
            OpusResult::InvalidRange : OpusResult::InvalidPacket};
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
    std::copy_n(samples_.data(), size_t(decoded) * config_.channels, pcm.data());
    return {OpusResult::Ok, bytes + 8, static_cast<uint32_t>(decoded), static_cast<uint64_t>(elapsed)};
}
}
