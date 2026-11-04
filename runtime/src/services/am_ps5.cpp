// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 application bootstrap. Command layouts follow am.cpp and libnx applet.c.
// Services are added as their state and backing are ported; unknown calls fail.
#include "services_internal.h"
#include "nxrt/kernel.h"
#include "nxrt/services_api.h"
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
Ptr proxy(const std::shared_ptr<State>& state) {
    return make_iface("IApplicationProxy", [state](Service& s) {
        s.add_cmd(0, "GetCommonStateGetter", [state](Request&, Response& r) -> uint32_t { r.push_object(common(state)); return 0; });
        s.add_cmd(1, "GetSelfController", [state](Request&, Response& r) -> uint32_t { r.push_object(self(state)); return 0; });
        s.add_cmd(2, "GetWindowController", [state](Request&, Response& r) -> uint32_t { r.push_object(window(state)); return 0; });
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
