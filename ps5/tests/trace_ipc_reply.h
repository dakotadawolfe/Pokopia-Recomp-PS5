// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nxrt/hipc.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>
namespace nxrt::diagnostic {
struct IpcArgumentSnapshot {
    bool valid = false, exact_size = false;
    size_t bytes = 0, captured = 0;
    std::array<uint8_t, 16> prefix{};
};
// Capture before dispatch overwrites TLS with its reply. Never follow buffer
// descriptors or retain guest pointers. Plain CMIF lengths include padding.
inline IpcArgumentSnapshot snapshot_arguments(const hipc::Message& message, bool domain) {
    IpcArgumentSnapshot result;
    size_t offset = 0, end = message.raw.size();
    if (hipc::is_tipc(message.type)) result.exact_size = true;
    else {
        if (message.type != hipc::kInvoke2MethodOld && message.type != hipc::kInvoke2Method) return result;
        if (domain) {
            if (end < 16 || message.raw[0] != 1) return result;
            uint16_t body_bytes = 0; std::memcpy(&body_bytes, message.raw.data() + 2, 2);
            if (body_bytes < 16 || body_bytes > end - 16) return result;
            offset = 16; end = 16 + body_bytes; result.exact_size = true;
        }
        if (end - offset < 16) return result;
        uint32_t magic = 0; std::memcpy(&magic, message.raw.data() + offset, 4);
        if (magic != 0x49434653) return result;
        offset += 16;
    }
    result.bytes = end - offset;
    result.captured = std::min(result.bytes, result.prefix.size());
    if (result.captured) std::memcpy(result.prefix.data(), message.raw.data() + offset, result.captured);
    result.valid = true;
    return result;
}
struct IpcReplyResult { bool valid = false; uint32_t result = 0, magic = 0; };
inline bool stop_on_reply(const IpcReplyResult& reply, std::string_view service, uint32_t command) {
    if (!reply.valid) return true;
    if (!reply.result) return false;
    // Implemented offline services must return their real error to the SDK so
    // it can take the offline path. Unknown commands still stop below and in
    // the unhandled-command audit; these are specific protocol result codes.
    if (service.starts_with("nn::nifm::") && reply.result == ((300u << 9) | 110u)) return false;
    if (service.starts_with("nn::ssl::") &&
        (reply.result == ((106u << 9) | 123u) || reply.result == ((126u << 9) | 123u))) return false;
    if (service.starts_with("nn::bcat::") && reply.result == ((1002u << 9) | 2u)) return false;
    if (service.starts_with("nn::ldn::") &&
        (reply.result == ((16u << 9) | 203u) || reply.result == ((32u << 9) | 203u) || reply.result == ((65u << 9) | 203u))) return false;
    if (service.starts_with("nn::ntc::") &&
        (reply.result == ((1000u << 9) | 116u) || reply.result == ((1003u << 9) | 116u))) return false;
    // These ported queue operations deliberately return an empty-queue result.
    // Pass it through unchanged so the SDK can finish polling. All other
    // failures, including missing game data and unported operations, still stop.
    if (service == "ICommonStateGetter" && command == 1 && reply.result == ((3u << 9) | 128u)) return false;
    if (service == "IPurchaseEventManager" && (command == 3 || command == 4) &&
        reply.result == ((400u << 9) | 164u)) return false;
    if (service == "IApplicationFunctions" && (command == 1 || command == 141 || command == 151) &&
        reply.result == ((2u << 9) | 128u)) return false;
    // Offline account operations return the actual registration-required result
    // to the SDK. Do not turn unported calls or malformed replies into success.
    if (reply.result == ((200u << 9) | 124u)) {
        if (service == "IManagerForApplication" && (command == 0 || command == 1 || command == 3 || command == 4)) return false;
        if ((service == "IAsyncContext" || service == "IAsyncNetworkServiceLicenseKindContext") && command == 3) return false;
    }
    return true;
}
inline IpcReplyResult decode_reply(const uint32_t* words, size_t count, bool tipc, size_t cmif_header = 0) {
    IpcReplyResult result;
    if (count < 2 || count > 1024 || (words[0] & 0xffff)) return result;
    std::vector<uint32_t> snapshot(words, words + count);
    // Both replies have type zero. TIPC raw data is unaligned, so retain the
    // request protocol when invoking the common message parser.
    if (tipc) snapshot[0] |= hipc::kTipcBase;
    hipc::Message message;
    if (!hipc::parse(snapshot.data(), snapshot.size(), message)) return result;
    const size_t offset = tipc ? 0 : cmif_header + 8;
    if (offset > message.raw.size() || message.raw.size() - offset < 4) return result;
    std::memcpy(&result.result, message.raw.data() + offset, 4);
    if (!tipc) std::memcpy(&result.magic, message.raw.data() + cmif_header, 4);
    result.valid = tipc || result.magic == 0x4f434653;
    return result;
}
}
