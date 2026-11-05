// SPDX-License-Identifier: GPL-3.0-or-later
// IPC layouts: libnx hwopus.c and Switchbrew Audio_services. Decoding is Xiph
// libopus; transfer-memory ownership follows the same lifetime as audio renderers.
#include "services_internal.h"
#include "nxrt/opus_decoder.h"
#include "nxrt/subpage.h"
#include "nxrt/log.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace nxrt::services {
namespace {
using namespace ipc;
constexpr uint32_t decode_failed = kernel::make_result(153, 2);
bool readable(const Buffer& b, uint64_t size) { return b.addr && size <= b.size && mem::SubpageMap::allows(b.addr, size, 1); }
bool parameters(const Request& req, bool multi, bool extended, audio::OpusConfig& cfg, bool opening) {
    cfg.multistream = multi;
    const uint8_t* data = req.in;
    const size_t required = multi ? (extended ? 0x118 : 0x110) : (extended ? 16 : 8);
    if (multi) {
        const auto buffer = req.in_buf(0);
        if (!readable(buffer, required)) return false;
        data = reinterpret_cast<const uint8_t*>(buffer.addr);
    } else if (!data || req.in_size < required) return false;
    std::memcpy(&cfg.rate, data, 4); std::memcpy(&cfg.channels, data + 4, 4);
    uint32_t large = 0;
    if (multi) {
        std::memcpy(&cfg.streams, data + 8, 4); std::memcpy(&cfg.coupled, data + 12, 4);
        if (extended) std::memcpy(&large, data + 16, 4);
        std::memcpy(cfg.mapping.data(), data + (extended ? 24 : 16), cfg.mapping.size());
    } else if (extended) std::memcpy(&large, data + 8, 4);
    if (large > 1) return false;
    cfg.large_frame = large != 0;
    return audio::OpusStream::valid(cfg, opening);
}
class Decoder final : public Service {
    std::shared_ptr<kernel::KTransferMemory> memory_;
    std::unique_ptr<audio::OpusStream> stream_;
    std::mutex mutex_;
public:
    Decoder(std::unique_ptr<audio::OpusStream> stream, std::shared_ptr<kernel::KTransferMemory> memory)
        : Service("nn::codec::detail::IHardwareOpusDecoder"), memory_(std::move(memory)), stream_(std::move(stream)) {
        for (uint32_t command : {0u, 2u, 4u, 5u, 6u, 7u, 8u, 9u})
            add_cmd(command, "DecodeInterleaved", [this, command](Request& req, Response& rsp) {
                std::lock_guard lock(mutex_);
                if (!stream_ || !memory_ || !memory_->host) return kernel::kInvalidState;
                const bool multi = command == 2 || command == 5 || command == 7 || command == 9;
                if (multi != stream_->config().multistream) return kernel::kInvalidState;
                const bool has_reset = command >= 6;
                if (has_reset && (!req.in || req.in_size < 1 || req.in[0] > 1)) return kernel::kInvalidSize;
                const auto in = req.in_buf(0), out = req.out_buf(0);
                if (!readable(in, 8)) return kernel::kInvalidPointer;
                uint8_t header[8]; std::memcpy(header, reinterpret_cast<const void*>(in.addr), 8);
                const uint64_t bytes = (uint32_t(header[0]) << 24 | uint32_t(header[1]) << 16 | uint32_t(header[2]) << 8 | header[3]);
                if (!bytes || bytes > in.size - 8 || bytes > 1024 * 1024 || !readable(in, bytes + 8)) return kernel::kInvalidSize;
                const auto& cfg = stream_->config();
                const uint64_t maximum = uint64_t(cfg.rate) * 120 / 1000 * cfg.channels * 2;
                const uint64_t output_size = std::min(out.size, maximum);
                if (!out.addr || out.addr % alignof(int16_t) || !output_size || !mem::SubpageMap::allows(out.addr, output_size, 2))
                    return kernel::kInvalidPointer;
                // Snapshot compressed data so an overlapping output descriptor
                // cannot overwrite the input while libopus reads it.
                std::vector<uint8_t> packet(bytes + 8);
                std::memcpy(packet.data(), reinterpret_cast<const void*>(in.addr), packet.size());
                const auto result = stream_->decode(packet,
                    {reinterpret_cast<int16_t*>(out.addr), static_cast<size_t>(output_size / 2)}, has_reset && req.in[0]);
                if (result.result != audio::OpusResult::Ok) return decode_failed;
                rsp.push<uint32_t>(result.consumed); rsp.push<uint32_t>(result.samples);
                if (command >= 4) rsp.push<uint64_t>(result.microseconds);
                return kernel::kOk;
            });
        for (uint32_t command : {1u, 3u}) add_cmd(command, "SetContext", [this, command](Request& req, Response&) {
            std::lock_guard lock(mutex_);
            if (!stream_ || (command == 3) != stream_->config().multistream) return kernel::kInvalidState;
            // This legacy request does not replace the Opus bitstream state.
            // The reset flag on DecodeInterleaved resets that state explicitly.
            const auto buffer = req.in_buf(0);
            return !buffer.size || readable(buffer, buffer.size) ? kernel::kOk : kernel::kInvalidPointer;
        });
    }
    void on_close() override { std::lock_guard lock(mutex_); stream_.reset(); memory_.reset(); }
};
std::shared_ptr<Service> manager() {
    return make_iface("nn::codec::detail::IHardwareOpusDecoderManager", [](Service& service) {
        for (uint32_t command = 0; command < 10; ++command) {
            const bool opening = command == 0 || command == 2 || command == 4 || command == 6;
            const bool multi = command == 2 || command == 3 || command == 6 || command == 7 || command == 9;
            const bool extended = command >= 4;
            service.add_cmd(command, opening ? "OpenHardwareOpusDecoder" : "GetWorkBufferSize",
                [=](Request& req, Response& rsp) -> uint32_t {
                    audio::OpusConfig cfg;
                    if (!parameters(req, multi, extended, cfg, opening)) return kernel::kInvalidEnum;
                    const uint32_t required = audio::OpusStream::work_size(cfg);
                    if (!required) return kernel::kInvalidSize;
                    if (!opening) { rsp.push<uint32_t>(required); return 0; }
                    const size_t size_offset = multi ? 0 : (extended ? 16 : 8);
                    if (!req.in || req.in_size < size_offset + 4) return kernel::kInvalidSize;
                    if (req.in_copy.size() != 1 || !req.in_move.empty()) return kernel::kInvalidHandle;
                    auto memory = kernel::handle_as<kernel::KTransferMemory>(req.in_copy[0]);
                    if (!memory || !memory->host) return kernel::kInvalidHandle;
                    const uint64_t size = req.arg<uint32_t>(size_offset);
                    if (size < required || size > memory->size) return kernel::kInvalidSize;
                    auto stream = audio::OpusStream::create(cfg);
                    if (!stream) return decode_failed;
                    NXLOG_I("hwopus", "Native Opus decoder: %d Hz, %d channels, multistream=%d", cfg.rate, cfg.channels, multi);
                    rsp.push_object(std::make_shared<Decoder>(std::move(stream), std::move(memory)));
                    return 0;
                });
        }
    });
}
}
void register_opus() { add("hwopus", manager); }
}
