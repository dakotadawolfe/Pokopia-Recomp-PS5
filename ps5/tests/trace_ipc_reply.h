// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nxrt/hipc.h"
#include <cstring>
#include <vector>
namespace nxrt::diagnostic {
struct IpcReplyResult { bool valid = false; uint32_t result = 0, magic = 0; };
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
