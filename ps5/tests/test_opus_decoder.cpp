// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/opus_decoder.h"
#include <opus.h>
#include <opus_multistream.h>
#include <algorithm>
#include <cmath>
#include <cstring>

NXTEST_GROUP(opus_decoder)
using nxrt::audio::OpusConfig;
using nxrt::audio::OpusStream;
using nxrt::audio::OpusResult;
namespace {
void put_be(std::vector<uint8_t>& packet, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) packet[offset+i] = value >> (24-8*i);
}
std::vector<uint8_t> tone(const OpusConfig& cfg, int frames, uint32_t* range = nullptr) {
    std::vector<int16_t> source(size_t(frames) * cfg.channels);
    for (int f = 0; f < frames; ++f) for (int c = 0; c < cfg.channels; ++c)
        source[f*cfg.channels+c] = static_cast<int16_t>(12000 * std::sin(f * 6.283185307179586 * (220 + 170*c) / cfg.rate));
    std::vector<uint8_t> result(8192); int error = 0, bytes = 0; opus_uint32 final_range = 0;
    if (cfg.multistream) {
        auto* enc = opus_multistream_encoder_create(cfg.rate, cfg.channels, cfg.streams, cfg.coupled, cfg.mapping.data(), OPUS_APPLICATION_AUDIO, &error);
        if (!enc || error) std::abort();
        opus_multistream_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
        bytes = opus_multistream_encode(enc, source.data(), frames, result.data()+8, result.size()-8);
        opus_multistream_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&final_range)); opus_multistream_encoder_destroy(enc);
    } else {
        auto* enc = opus_encoder_create(cfg.rate, cfg.channels, OPUS_APPLICATION_AUDIO, &error);
        if (!enc || error) std::abort();
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
        bytes = opus_encode(enc, source.data(), frames, result.data()+8, result.size()-8);
        opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&final_range)); opus_encoder_destroy(enc);
    }
    if (bytes <= 0) std::abort();
    result.resize(bytes+8); put_be(result, 0, bytes); put_be(result, 4, final_range);
    if (range) *range = final_range;
    return result;
}
}
NXTEST(opus_decodes_real_stereo_pcm_and_reset_reproduces_first_packet) {
    OpusConfig cfg; auto stream = OpusStream::create(cfg); REQUIRE(stream);
    auto packet = tone(cfg, 960); std::vector<int16_t> output(2000, 12345), first;
    const auto result = stream->decode(packet, output, false);
    CHECK(result.result == OpusResult::Ok); CHECK_EQ(result.samples, 960u); CHECK_EQ(result.consumed, packet.size());
    uint64_t energy = 0; for (size_t i = 0; i < 1920; ++i) energy += std::abs(int(output[i]));
    CHECK(energy > 100000); CHECK_EQ(output[1920], 12345); CHECK_EQ(output.back(), 12345);
    first = output; CHECK(stream->decode(packet, output, true).result == OpusResult::Ok); CHECK(output == first);
}
NXTEST(opus_bad_packet_range_and_small_output_do_not_modify_pcm_or_decoder) {
    OpusConfig cfg; auto packet = tone(cfg, 960); auto stream = OpusStream::create(cfg); REQUIRE(stream);
    std::vector<int16_t> output(1920, 9876); auto corrupt = packet; corrupt[0] = 0xff;
    CHECK(stream->decode(corrupt, output, false).result == OpusResult::InvalidPacket);
    CHECK(stream->decode(packet, {output.data(), 100}, false).result == OpusResult::BufferTooSmall);
    corrupt = packet; corrupt[7] ^= 0x80;
    CHECK(stream->decode(corrupt, output, false).result == OpusResult::InvalidRange);
    CHECK(std::all_of(output.begin(), output.end(), [](auto v) { return v == 9876; }));
    auto reference = OpusStream::create(cfg); std::vector<int16_t> expected(1920);
    REQUIRE(reference->decode(packet, expected, false).result == OpusResult::Ok);
    REQUIRE(stream->decode(packet, output, false).result == OpusResult::Ok); CHECK(output == expected);
}
NXTEST(opus_multistream_decodes_six_independent_channels) {
    OpusConfig cfg; cfg.multistream = true; cfg.channels = 6; cfg.streams = cfg.coupled = 3;
    for (unsigned n = 0; n < 6; ++n) cfg.mapping[n] = n;
    auto stream = OpusStream::create(cfg); REQUIRE(stream);
    auto packet = tone(cfg, 960); std::vector<int16_t> output(5760);
    auto result = stream->decode(packet, output, false); REQUIRE(result.result == OpusResult::Ok); CHECK_EQ(result.samples, 960u);
    for (unsigned c = 0; c < 6; ++c) { uint64_t energy = 0; for (unsigned f = 0; f < 960; ++f) energy += std::abs(int(output[f*6+c])); CHECK(energy > 100000); }
    CHECK(output[3000] != output[3001]);
}
NXTEST(opus_parameter_and_large_frame_limits) {
    OpusConfig cfg; cfg.rate = 44100; CHECK(!OpusStream::valid(cfg)); CHECK_EQ(OpusStream::work_size(cfg), 0u);
    cfg.rate = 48000; cfg.channels = 3; CHECK(!OpusStream::create(cfg));
    cfg.channels = 2; const auto small_size = OpusStream::work_size(cfg); CHECK_EQ(small_size % 4096, 0u);
    auto ordinary = OpusStream::create(cfg); auto packet = tone(cfg, 2880); std::vector<int16_t> pcm(5760);
    CHECK(ordinary->decode(packet, pcm, false).result == OpusResult::BufferTooSmall);
    cfg.large_frame = true; CHECK(OpusStream::work_size(cfg) > small_size); auto large = OpusStream::create(cfg);
    REQUIRE(large->decode(packet, pcm, false).result == OpusResult::Ok);
    cfg.multistream = true; cfg.mapping[0] = 9; CHECK(!OpusStream::valid(cfg));
    cfg.mapping[0] = 0; cfg.streams = 0; CHECK(!OpusStream::valid(cfg));
}
