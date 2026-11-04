// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 application bootstrap. Command layouts follow am.cpp and libnx applet.c.
// Services are added as their state and backing are ported; unknown calls fail.
#include "services_internal.h"
#include "nxrt/kernel.h"
#include "nxrt/subpage.h"
#include "nxrt/services_api.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace nxrt::services {
namespace {
using ipc::Request;
using ipc::Response;
using ipc::Service;
using Ptr = std::shared_ptr<Service>;
constexpr uint32_t focus_changed = 15, operation_changed = 30, performance_changed = 31;
struct State {
    std::mutex mu;
    std::shared_ptr<kernel::KProcess> process = kernel::current_process();
    std::shared_ptr<kernel::EventState> message_event = std::make_shared<kernel::EventState>();
    std::deque<uint32_t> messages{focus_changed};
    bool docked = true, focused = true, foreground = false;
    bool operation_notify = false, performance_notify = false;
    bool graphics_notify = false;
    bool exit_locked = false, fatal_section = false;
    uint32_t terminate_result = 0;
    float main_volume = 1.0f, library_volume = 1.0f;
    State() { message_event->signaled = true; }
    void queue(uint32_t value) {
        messages.push_back(value); message_event->signaled = true; kernel::notify_waitable_changed();
    }
};
std::mutex state_mu;
std::shared_ptr<State> state;
std::shared_ptr<State> current() {
    std::lock_guard lock(state_mu);
    if (!state) state = std::make_shared<State>();
    return state;
}
uint32_t event_handle(const std::shared_ptr<kernel::EventState>& event, Response& rsp) {
    auto object = std::make_shared<kernel::KEvent>(); object->state = event;
    const auto handle = kernel::handle_add(object);
    if (!handle) return kernel::kOutOfHandles;
    rsp.out_copy.push_back(handle); return 0;
}
Ptr common(const std::shared_ptr<State>& state) {
    return make_iface("ICommonStateGetter", [state](Service& s) {
        s.add_cmd(0, "GetEventHandle", [state](Request&, Response& rsp) {
            return event_handle(state->message_event, rsp);
        });
        s.add_cmd(1, "ReceiveMessage", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu);
            if (state->messages.empty()) return res(128, 3);
            rsp.push(state->messages.front()); state->messages.pop_front();
            state->message_event->signaled = !state->messages.empty(); return 0;
        });
        s.add_cmd(5, "GetOperationMode", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu); rsp.push<uint8_t>(state->docked); return 0;
        });
        s.add_cmd(6, "GetPerformanceMode", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu); rsp.push<uint32_t>(state->docked); return 0;
        });
        s.add_cmd(9, "GetCurrentFocusState", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu); rsp.push<uint8_t>(state->focused ? 1 : 2); return 0;
        });
        s.add_cmd(130, "EnableStartupLogoDisappearedMessage", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->queue(95); return 0;
        });
    });
}
Ptr self(const std::shared_ptr<State>& state) {
    return make_iface("ISelfController", [state](Service& s) {
        s.add_cmd(1, "LockExit", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->exit_locked = true; return 0;
        });
        s.add_cmd(2, "UnlockExit", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->exit_locked = false; return 0;
        });
        s.add_cmd(3, "EnterFatalSection", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->fatal_section = true; return 0;
        });
        s.add_cmd(4, "LeaveFatalSection", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->fatal_section = false; return 0;
        });
        s.add_cmd(11, "SetOperationModeChangedNotification", [state](Request& req, Response&) -> uint32_t {
            if (req.in_size < 1) return kernel::kInvalidCombination;
            std::lock_guard lock(state->mu); state->operation_notify = req.arg<uint8_t>(0) != 0; return 0;
        });
        s.add_cmd(12, "SetPerformanceModeChangedNotification", [state](Request& req, Response&) -> uint32_t {
            if (req.in_size < 1) return kernel::kInvalidCombination;
            std::lock_guard lock(state->mu); state->performance_notify = req.arg<uint8_t>(0) != 0; return 0;
        });
    });
}
Ptr window(const std::shared_ptr<State>& state) {
    return make_iface("IWindowController", [state](Service& s) {
        s.add_cmd(1, "GetAppletResourceUserId", [state](Request&, Response& rsp) -> uint32_t {
            rsp.push(state->process->id); return 0;
        });
        s.add_cmd(10, "AcquireForegroundRights", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->foreground = true; return 0;
        });
        s.add_cmd(11, "ReleaseForegroundRights", [state](Request&, Response&) -> uint32_t {
            std::lock_guard lock(state->mu); state->foreground = false; return 0;
        });
    });
}
Ptr application(const std::shared_ptr<State>& state) {
    return make_iface("IApplicationFunctions", [state](Service& s) {
        s.add_cmd(22, "SetTerminateResult", [state](Request& req, Response&) -> uint32_t {
            if (req.in_size < 4) return kernel::kInvalidCombination;
            std::lock_guard lock(state->mu); state->terminate_result = req.arg<uint32_t>(0); return 0;
        });
    });
}
struct Storage {
    std::mutex mu;
    std::vector<uint8_t> bytes;
    explicit Storage(size_t size) : bytes(size) {}
};
Ptr accessor(const std::shared_ptr<Storage>& data) {
    return make_iface("IStorageAccessor", [data](Service& s) {
        s.add_cmd(0, "GetSize", [data](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(data->bytes.size()); return 0;
        });
        const auto transfer = [data](Request& req, bool write) -> uint32_t {
            if (req.in_size < sizeof(int64_t)) return kernel::kInvalidCombination;
            const int64_t offset = req.arg<int64_t>(0);
            const auto buffer = write ? req.in_buf(0) : req.out_buf(0);
            if (offset < 0 || static_cast<uint64_t>(offset) > data->bytes.size() ||
                buffer.size > data->bytes.size() - static_cast<uint64_t>(offset)) return res(128, 503);
            if (!buffer.size) return 0;
            if (!mem::SubpageMap::allows(buffer.addr, buffer.size, write ? 1 : 2)) return kernel::kInvalidMemoryState;
            std::lock_guard lock(data->mu);
            auto* bytes = data->bytes.data() + offset;
            if (write) std::memcpy(bytes, reinterpret_cast<const void*>(buffer.addr), buffer.size);
            else std::memcpy(reinterpret_cast<void*>(buffer.addr), bytes, buffer.size);
            return 0;
        };
        s.add_cmd(10, "Write", [transfer](Request& req, Response&) { return transfer(req, true); });
        s.add_cmd(11, "Read", [transfer](Request& req, Response&) { return transfer(req, false); });
    });
}
Ptr library_creator() {
    return make_iface("ILibraryAppletCreator", [](Service& s) {
        s.add_cmd(10, "CreateStorage", [](Request& req, Response& rsp) -> uint32_t {
            if (req.in_size < sizeof(int64_t)) return kernel::kInvalidCombination;
            const auto size = req.arg<int64_t>(0);
            if (size < 0 || size > 0x10000000) return res(128, 600);
            try {
                auto data = std::make_shared<Storage>(static_cast<size_t>(size));
                rsp.push_object(make_iface("IStorage", [data](Service& storage) {
                    storage.add_cmd(0, "Open", [data](Request&, Response& out) -> uint32_t {
                        out.push_object(accessor(data)); return 0;
                    });
                }));
                return 0;
            } catch (const std::bad_alloc&) { return res(128, 600); }
        });
        // UI applets and transfer-memory storage need their own backing before
        // they can be created. Keep these failures distinct from an absent command.
        for (const auto cmd : {0u, 3u, 11u, 12u})
            s.add_cmd(cmd, "UnportedAppletOrTransferStorage", [](Request&, Response&) -> uint32_t {
                return kernel::kNotImplemented;
            });
    });
}
Ptr audio(const std::shared_ptr<State>& state) {
    return make_iface("IAudioController", [state](Service& s) {
        s.add_cmd(0, "SetExpectedMasterVolume", [state](Request& req, Response&) -> uint32_t {
            if (req.in_size < 8) return kernel::kInvalidCombination;
            const auto main = req.arg<float>(0), library = req.arg<float>(4);
            if (!std::isfinite(main) || !std::isfinite(library)) return kernel::kOutOfRange;
            std::lock_guard lock(state->mu);
            state->main_volume = std::clamp(main, 0.0f, 1.0f);
            state->library_volume = std::clamp(library, 0.0f, 1.0f); return 0;
        });
        s.add_cmd(1, "GetMainAppletExpectedMasterVolume", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu); rsp.push(state->main_volume); return 0;
        });
        s.add_cmd(2, "GetLibraryAppletExpectedMasterVolume", [state](Request&, Response& rsp) -> uint32_t {
            std::lock_guard lock(state->mu); rsp.push(state->library_volume); return 0;
        });
        // Expected volume is queryable state; output mixing and fades are unported.
    });
}
Ptr proxy(const std::shared_ptr<State>& state) {
    return make_iface("IApplicationProxy", [state](Service& s) {
        s.add_cmd(0, "GetCommonStateGetter", [state](Request&, Response& r) -> uint32_t { r.push_object(common(state)); return 0; });
        s.add_cmd(1, "GetSelfController", [state](Request&, Response& r) -> uint32_t { r.push_object(self(state)); return 0; });
        s.add_cmd(2, "GetWindowController", [state](Request&, Response& r) -> uint32_t { r.push_object(window(state)); return 0; });
        s.add_cmd(3, "GetAudioController", [state](Request&, Response& r) -> uint32_t { r.push_object(audio(state)); return 0; });
        s.add_cmd(11, "GetLibraryAppletCreator", [](Request&, Response& r) -> uint32_t { r.push_object(library_creator()); return 0; });
        s.add_cmd(20, "GetApplicationFunctions", [state](Request&, Response& r) -> uint32_t { r.push_object(application(state)); return 0; });
        s.add_cmd(40, "GetPerformanceFunctions", [](Request&, Response& r) -> uint32_t {
            r.push_object(make_iface("IPerformanceFunctions", [](Service& g) {
                g.add_cmd(0, "IsCpuOverclockEnabled", [](Request&, Response& out) -> uint32_t { out.push<uint8_t>(0); return 0; });
                g.add_cmd(1, "SetCpuOverclockEnabled", [](Request& req, Response&) -> uint32_t {
                    if (req.in_size < 1) return kernel::kInvalidCombination;
                    return req.arg<uint8_t>(0) ? kernel::kNotImplemented : 0;
                });
            })); return 0;
        });
        s.add_cmd(60, "GetGraphicsScalingParametersFunctions", [state](Request&, Response& r) -> uint32_t {
            r.push_object(make_iface("IGraphicsScalingParametersFunctions", [state](Service& g) {
                g.add_cmd(10, "GetGraphicsScalingParameters", [](Request&, Response& out) -> uint32_t {
                    // Same full-quality guest scale as am.cpp, fixed for this port.
                    out.push<uint32_t>(10); out.push<uint32_t>(0); out.push<uint64_t>(0); return 0;
                });
                g.add_cmd(1050, "SetGraphicsScaleChangedNotificationEnabled", [state](Request& req, Response&) -> uint32_t {
                    if (req.in_size < 1) return kernel::kInvalidCombination;
                    std::lock_guard lock(state->mu); state->graphics_notify = req.arg<uint8_t>(0) != 0; return 0;
                });
            })); return 0;
        });
    });
}
uint32_t open_proxy(Request& req, Response& rsp) {
    const auto state = current();
    if (req.in_copy.size() != 1 || !req.in_move.empty()) return kernel::kInvalidHandle;
    const auto process = kernel::handle_as<kernel::KProcess>(req.in_copy[0]);
    if (process != state->process || req.pid != process->id) return kernel::kInvalidHandle;
    rsp.push_object(proxy(state)); return 0;
}
}
void register_am() {
    add("appletOE", [] { return make_iface("appletOE", [](Service& s) {
        s.add_cmd(0, "OpenApplicationProxy", open_proxy);
        s.add_cmd(10, "OpenApplicationProxy2", open_proxy);
    }); });
}
void am_reset() { std::lock_guard lock(state_mu); state.reset(); }
namespace amctl {
void set_docked(bool docked) {
    auto s = current(); std::lock_guard lock(s->mu);
    if (s->docked == docked) return;
    s->docked = docked;
    if (s->operation_notify) s->queue(operation_changed);
    if (s->performance_notify) s->queue(performance_changed);
}
void set_focus(bool focused) {
    auto s = current(); std::lock_guard lock(s->mu);
    if (s->focused == focused) return;
    s->focused = focused; s->queue(focus_changed);
}
bool docked() { auto s = current(); std::lock_guard lock(s->mu); return s->docked; }
bool in_focus() { auto s = current(); std::lock_guard lock(s->mu); return s->focused; }
}
} // namespace nxrt::services
