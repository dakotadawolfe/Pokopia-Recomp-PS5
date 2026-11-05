// SPDX-License-Identifier: GPL-3.0-or-later
// Offline startup portion of the upstream misc.cpp streamplay service.
// The command layouts and empty guest-HID format are shared with that core.
// Network sessions, matchmaking and actual streaming remain unsupported.
#include "services_internal.h"
#include <array>
#include <cmath>
#include <mutex>

namespace nxrt::services {
namespace {
constexpr uint64_t hid_bytes = 0x84000, work_page = 0x1000;
struct State {
    std::mutex mutex;
    std::unique_ptr<ServiceSharedMemory> hid;
    ServiceEvent connection_changed{false};
    bool initialized = false;
    uint8_t type = 0, mode = 0;
    uint32_t guests = 0, styles = 0, hold_type = 0;
    std::array<float, 4> volume{};
    void reset() {
        initialized = false; type = mode = 0; guests = styles = hold_type = 0;
        volume.fill(0); connection_changed.clear();
        // HID memory belongs to the process's user service, not one session.
        // Existing guest mappings and duplicated handles remain valid.
    }
};
using namespace ipc;
bool valid_setup(const Request& req) {
    return req.in && req.in_size >= 8 && req.arg<uint8_t>(0) <= 1 && req.arg<uint32_t>(4) <= 3;
}
std::shared_ptr<Service> user_service(std::shared_ptr<State> state) {
    return make_iface("nn::streamplay::detail::IUserService", [state](Service& s) {
        s.add_cmd(1, "GetRequiredMemorySize", [](Request& req, Response& rsp) -> uint32_t {
            if (!valid_setup(req)) return kernel::kInvalidSize;
            const uint32_t guests = req.arg<uint32_t>(4);
            rsp.push<uint32_t>(req.arg<uint8_t>(0) == 1 ? guests * work_page : work_page + guests * 3 * work_page);
            return 0;
        });
        s.add_cmd(9, "GetWorkBufferSizes", [](Request& req, Response& rsp) -> uint32_t {
            if (!req.in || req.in_size < 2 || req.arg<uint8_t>(0) > 1) return kernel::kInvalidSize;
            for (unsigned n = 0; n < 4; ++n) rsp.push<uint64_t>(work_page);
            return 0;
        });
        s.add_cmd(300, "GetHidSharedMemory", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mutex);
            if (!state->hid) {
                auto memory = std::make_unique<ServiceSharedMemory>(hid_bytes, "streamplay.hid");
                if (!memory->ok()) return kernel::kOutOfMemory;
                state->hid = std::move(memory);
            }
            const uint32_t handle = state->hid->new_handle();
            if (!handle) return kernel::kOutOfHandles;
            rsp.push<uint64_t>(hid_bytes); rsp.push<uint64_t>(0);
            rsp.out_copy.push_back(handle);
            return 0;
        });
        s.add_cmd(2, "Initialize", [state](Request& req, Response&) -> uint32_t {
            if (!valid_setup(req)) return kernel::kInvalidSize;
            std::lock_guard lock(state->mutex);
            if (state->initialized) return kernel::kInvalidState;
            state->type = req.arg<uint8_t>(0); state->mode = req.arg<uint8_t>(1);
            state->guests = req.arg<uint32_t>(4); state->initialized = true;
            return 0;
        });
        s.add_cmd(3, "Finalize", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mutex); state->reset(); return 0;
        });
        s.add_cmd(5, "CloseSession", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mutex); state->connection_changed.clear(); return 0;
        });
        s.add_cmd(7, "GetConnectionStateChangedEvent", [state](Request&, Response& rsp) -> uint32_t {
            const auto handle = state->connection_changed.new_handle();
            if (!handle) return kernel::kOutOfHandles;
            rsp.out_copy.push_back(handle); return 0;
        });
        s.add_cmd(8, "GetConnectionState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0); return 0; // Disconnected, no pending result.
        });
        s.add_cmd(25, "SetSupportedGuestNpadStyleSet", [state](Request& req, Response&) -> uint32_t {
            if (!req.in || req.in_size < 4) return kernel::kInvalidSize;
            std::lock_guard lock(state->mutex); state->styles = req.arg<uint32_t>(0); return 0;
        });
        s.add_cmd(27, "SetGuestNpadJoyHoldType", [state](Request& req, Response&) -> uint32_t {
            if (!req.in || req.in_size < 4) return kernel::kInvalidSize;
            std::lock_guard lock(state->mutex); state->hold_type = req.arg<uint32_t>(0); return 0;
        });
        s.add_cmd(502, "GetStreamPlayType", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mutex); rsp.push<uint8_t>(state->type); return 0;
        });
        s.add_cmd(503, "GetStreamingMode", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mutex); rsp.push<uint8_t>(state->mode); return 0;
        });
        s.add_cmd(510, "SetAudioOutputVolumes", [state](Request& req, Response&) -> uint32_t {
            if (!req.in || req.in_size < 16) return kernel::kInvalidSize;
            std::array<float, 4> values;
            for (size_t n = 0; n < values.size(); ++n) {
                values[n] = req.arg<float>(n * sizeof(float));
                if (!std::isfinite(values[n])) return kernel::kInvalidSize;
            }
            std::lock_guard lock(state->mutex); state->volume = values; return 0;
        });
    });
}
}
void register_streamplay() {
    auto state = std::make_shared<State>();
    add("stpl:u", [state] {
        return make_iface("stpl:u", [state](ipc::Service& s) {
            s.add_cmd(0, "CreateUserService", [state](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(user_service(state)); return 0;
            });
        });
    });
}
}
