// SPDX-License-Identifier: GPL-3.0-or-later
// IPC SVCs: ConnectToNamedPort (0x1F), SendSyncRequest (0x21), SendSyncRequestWithUserBuffer (0x22). Calling convention: svc.h.
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/svc.h"
#ifndef _WIN32
#include "nxrt/hipc.h"
#include "nxrt/subpage.h"
#include <array>
#include <cstring>
#endif

namespace nxrt {
namespace {

constexpr uint64_t kTlsMessageBytes = 0x100;   // the message buffer at TPIDRRO_EL0 is 0x100 bytes

#ifndef _WIN32
uint32_t checked_send(::Ctx* c, uint32_t session, uint64_t address, uint64_t size) {
    if (!kernel::handle_as<ipc::KSession>(session)) return kernel::kInvalidHandle;
    if (!mem::SubpageMap::allows(address, size, 3)) return kernel::kInvalidMemoryState;
    // Snapshot the message so a guest write cannot replace descriptors between
    // validation and dispatch. The caller must still coordinate unmaps.
    alignas(16) std::array<uint32_t, 0x1000 / 4> words{};
    std::memcpy(words.data(), reinterpret_cast<const void*>(address), size);
    hipc::Message message;
    if (!hipc::parse(words.data(), size / 4, message)) return kernel::kInvalidCombination;
    const auto allows = [](const auto& descriptors, uint8_t permission) {
        for (const auto& d : descriptors)
            if (d.size && !mem::SubpageMap::allows(d.addr, d.size, permission)) return false;
        return true;
    };
    if (!allows(message.send, 1) || !allows(message.ptrs, 1) || !allows(message.recv, 2) ||
        !allows(message.recv_list, 2) || !allows(message.exch, 3)) return kernel::kInvalidMemoryState;
    std::vector<std::shared_ptr<kernel::KObject>> copied;
    for (auto handle : message.copy) {
        auto object = kernel::handle_get(handle);
        if (!object) return kernel::kInvalidHandle;
        copied.push_back(std::move(object));
    }
    for (auto handle : message.move) {
        if (handle == kernel::kPseudoCurrentProcess || handle == kernel::kPseudoCurrentThread || !kernel::handle_get(handle))
            return kernel::kInvalidHandle;
    }
    struct CopyHandles {
        std::vector<uint32_t> handles;
        ~CopyHandles() { for (auto h : handles) kernel::handle_close(h); }
    } received;
    received.handles.reserve(copied.size());
    for (size_t i = 0; i < copied.size(); ++i) {
        const auto handle = kernel::handle_add(copied[i]);
        if (!handle) return kernel::kOutOfHandles;
        received.handles.push_back(handle); message.copy[i] = handle;
    }
    // Rewrite only the validated special header. Rebuilding a parsed message
    // would add CMIF padding a second time and move its receive-list descriptors.
    size_t special_word = 3;
    if (message.has_pid) {
        const auto pid = kernel::current_process()->id;
        words[special_word++] = static_cast<uint32_t>(pid);
        words[special_word++] = static_cast<uint32_t>(pid >> 32);
    }
    for (auto handle : message.copy) words[special_word++] = handle;
    const auto result = ipc::send_sync_request(c, session, reinterpret_cast<uint64_t>(words.data()), size);
    if (result == kernel::kOk) std::memcpy(reinterpret_cast<void*>(address), words.data(), size);
    return result;
}
#endif

// ConnectToNamedPort(const char* name) -> (result, handle in x1). The name is read up to 12 bytes.
void svc_connect_to_named_port(::Ctx* c) {
    uint32_t h = 0;
#ifdef _WIN32
    const uint32_t rc = ipc::connect_named_port(reinterpret_cast<const char*>(static_cast<uintptr_t>(c->x[1])), &h);
#else
    char name[12]{};
    const auto address = c->x[1];
    c->x[1] = 0;
    bool terminated = false;
    for (size_t i = 0; i < sizeof name; ++i) {
        if (address > UINT64_MAX - i || !mem::SubpageMap::allows(address + i, 1, 1))
            return svc_ret(c, kernel::kInvalidPointer);
        name[i] = *reinterpret_cast<const char*>(address + i);
        if (!name[i]) { terminated = true; break; }
    }
    if (!terminated) return svc_ret(c, kernel::kOutOfRange);
    if (!ipc::has_service(name)) return svc_ret(c, kernel::kNotFound);
    const uint32_t rc = ipc::connect_named_port(name, &h);
#endif
    c->x[1] = h;
    svc_ret(c, rc);
}

// SendSyncRequest(Handle session) with the request in the calling thread's TLS message buffer (TPIDRRO_EL0).
void svc_send_sync_request(::Ctx* c) {
    const uint32_t session = static_cast<uint32_t>(c->x[0]);
#ifdef _WIN32
    svc_ret(c, ipc::send_sync_request(c, session, c->tpidrro, kTlsMessageBytes));
#else
    svc_ret(c, checked_send(c, session, c->tpidrro, kTlsMessageBytes));
#endif
}

// SendSyncRequestWithUserBuffer(void* buffer, size_t size, Handle session).
void svc_send_sync_request_with_user_buffer(::Ctx* c) {
    const uint32_t session = static_cast<uint32_t>(c->x[2]);
#ifdef _WIN32
    svc_ret(c, ipc::send_sync_request(c, session, c->x[0], c->x[1]));
#else
    const uint64_t address = c->x[0], size = c->x[1];
    if (address & 0xfff) return svc_ret(c, kernel::kInvalidAddress);
    if (!size || (size & 0xfff)) return svc_ret(c, kernel::kInvalidSize);
    if (address > UINT64_MAX - size) return svc_ret(c, kernel::kInvalidMemoryState);
    if (size > 0x1000) return svc_ret(c, kernel::kNotImplemented);
    svc_ret(c, checked_send(c, session, address, size));
#endif
}

}  // namespace

void svc_register_ipc_group() {
    svc_register(0x1F, svc_connect_to_named_port);
    svc_register(0x21, svc_send_sync_request);
    svc_register(0x22, svc_send_sync_request_with_user_buffer);
}

}  // namespace nxrt
