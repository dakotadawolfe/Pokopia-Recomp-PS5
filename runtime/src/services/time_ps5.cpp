// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only time:u, using the existing Horizon clock/layout and timezone rules.
// Host UTC seeds a steady guest clock; administrative clocks are not registered.
#include "services_internal.h"
#include "time_clock.h"
#include "timezone.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/subpage.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace nxrt::ipc::detail { uint64_t note_unhandled(Service&, uint32_t); }

namespace nxrt::services {
namespace {
using ipc::Request; using ipc::Response; using ipc::Service;
using Ptr = std::shared_ptr<Service>;
namespace clock = time_clock;
constexpr uint32_t permission_denied = res(116, 1), out_of_range = res(116, 902);
constexpr uint32_t conversion_failed = res(116, 903), zone_not_found = res(116, 989);

class TimeService final : public Service {
public:
    explicit TimeService(const char* name) : Service(name) {}
    uint32_t handle(Request& req, Response& rsp) override {
        if (cmds_.contains(req.cmd)) return Service::handle(req, rsp);
        if (ipc::detail::note_unhandled(*this, req.cmd) == 1)
            NXLOG_W("time", "%s cmd %u is not ported", name().c_str(), req.cmd);
        return kernel::kNotImplemented;
    }
};
struct State {
    clock::Clock clock;
    bool ready = false;
    std::mutex mutex;
    std::shared_ptr<kernel::KSharedMemory> shared;
    const std::shared_ptr<kernel::EventState> clock_event = std::make_shared<kernel::EventState>();
    const std::shared_ptr<kernel::EventState> zone_event = std::make_shared<kernel::EventState>();
    std::array<uint8_t, tz::kRuleSize> utc{};
    State() {
        static std::atomic<uint64_t> generation{0};
        const auto monotonic = std::chrono::steady_clock::now().time_since_epoch();
        const auto nonce = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(monotonic).count()) ^
                           (++generation * 0x9e3779b97f4a7c15ull);
        clock::SourceId source{'n', 'x', 'p', 's', '5', 't', 'm', 0};
        std::memcpy(source.data() + 8, &nonce, 8);
        const auto unix_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ready = clock.initialize(kernel::system_tick(), kernel::tick_hz(), unix_time, source);
        tz::make_utc_rule(utc.data());
    }
};
std::mutex state_mutex;
std::shared_ptr<State> state;
std::shared_ptr<State> current() {
    std::lock_guard lock(state_mutex);
    if (!state) state = std::make_shared<State>();
    return state;
}
bool arguments(const Request& req, size_t size) { return req.in && req.in_size >= size; }
uint32_t event_handle(const std::shared_ptr<kernel::EventState>& event, Response& rsp) {
    auto object = std::make_shared<kernel::KEvent>(); object->state = event;
    const auto handle = kernel::handle_add(object);
    if (!handle) return kernel::kOutOfHandles;
    rsp.out_copy.push_back(handle); return 0;
}
uint32_t output(const ipc::Buffer& buffer, const void* data, size_t size) {
    if (buffer.size < size || (size && !buffer.addr)) return out_of_range;
    if (size && !mem::SubpageMap::allows(buffer.addr, size, 2)) return kernel::kInvalidMemoryState;
    if (size) std::memcpy(reinterpret_cast<void*>(buffer.addr), data, size);
    return 0;
}
uint32_t input_rule(const Request& req, std::array<uint8_t, tz::kRuleSize>& rule) {
    const auto input = req.in_buf(0);
    if (!input.addr || input.size < rule.size()) return conversion_failed;
    if (!mem::SubpageMap::allows(input.addr, rule.size(), 1)) return kernel::kInvalidMemoryState;
    std::memcpy(rule.data(), reinterpret_cast<const void*>(input.addr), rule.size());
    return tz::rule_valid(rule.data()) ? 0 : conversion_failed;
}
uint32_t denied(Request&, Response&) { return permission_denied; }

Ptr system_clock(const std::shared_ptr<State>& state) {
    auto service = std::make_shared<TimeService>("ISystemClock");
    service->add_cmd(0, "GetCurrentTime", [state](Request&, Response& rsp) -> uint32_t {
        int64_t time = 0;
        if (!state->clock.sample(kernel::system_tick(), nullptr, &time)) return kernel::kInvalidState;
        rsp.push(time); return 0;
    });
    service->add_cmd(1, "SetCurrentTime", denied);
    service->add_cmd(2, "GetSystemClockContext", [state](Request&, Response& rsp) -> uint32_t {
        clock::SystemContext context{};
        if (!state->clock.context(kernel::system_tick(), &context)) return kernel::kInvalidState;
        rsp.push(context); return 0;
    });
    service->add_cmd(3, "SetSystemClockContext", denied);
    service->add_cmd(4, "GetOperationEventReadableHandle", [state](Request&, Response& rsp) {
        return event_handle(state->clock_event, rsp);
    });
    return service;
}
Ptr steady_clock(const std::shared_ptr<State>& state) {
    auto service = std::make_shared<TimeService>("ISteadyClock");
    service->add_cmd(0, "GetCurrentTimePoint", [state](Request&, Response& rsp) -> uint32_t {
        clock::TimePoint point{};
        if (!state->clock.sample(kernel::system_tick(), &point, nullptr)) return kernel::kInvalidState;
        rsp.push(point); return 0;
    });
    for (const auto command : {2u, 200u})
        service->add_cmd(command, "GetClockOffset", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(0); return 0;
        });
    service->add_cmd(3, "SetTestOffset", denied);
    service->add_cmd(100, "GetRtcValue", [state](Request&, Response& rsp) -> uint32_t {
        int64_t time = 0;
        if (!state->clock.sample(kernel::system_tick(), nullptr, &time)) return kernel::kInvalidState;
        if (time < 946684800) return out_of_range;
        rsp.push<uint64_t>(static_cast<uint64_t>(time - 946684800)); return 0;
    });
    service->add_cmd(102, "GetSetupResultValue", [state](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(state->ready ? 0 : kernel::kInvalidState); return 0;
    });
    return service;
}
Ptr zone(const std::shared_ptr<State>& state) {
    auto service = std::make_shared<TimeService>("ITimeZoneService");
    service->add_cmd(0, "GetDeviceLocationName", [](Request&, Response& rsp) -> uint32_t {
        const std::array<char, 0x24> name{'U', 'T', 'C'}; rsp.push(name); return 0;
    });
    service->add_cmd(1, "SetDeviceLocationName", denied);
    service->add_cmd(2, "GetTotalLocationNameCount", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<int32_t>(1); return 0;
    });
    service->add_cmd(3, "LoadLocationNameList", [](Request& req, Response& rsp) -> uint32_t {
        if (!arguments(req, 4)) return out_of_range;
        const auto index = req.arg<int32_t>(0);
        if (index < 0) return out_of_range;
        const std::array<char, 0x24> name{'U', 'T', 'C'};
        int32_t count = 0;
        if (index == 0 && req.out_buf(0).size >= name.size()) {
            const auto result = output(req.out_buf(0), name.data(), name.size());
            if (result) return result;
            count = 1;
        }
        rsp.push(count); return 0;
    });
    service->add_cmd(4, "LoadTimeZoneRule", [state](Request& req, Response&) -> uint32_t {
        if (!arguments(req, 0x24)) return out_of_range;
        const auto name = req.arg<std::array<char, 0x24>>(0);
        if (std::memcmp(name.data(), "UTC\0", 4) != 0) return zone_not_found;
        if (req.out_buf(0).size != tz::kRuleSize) return out_of_range;
        return output(req.out_buf(0), state->utc.data(), state->utc.size());
    });
    service->add_cmd(6, "GetDeviceLocationNameAndUpdatedTime", [state](Request&, Response& rsp) -> uint32_t {
        const std::array<char, 0x28> name{'U', 'T', 'C'}; rsp.push(name);
        rsp.push(clock::TimePoint{clock::steady_epoch, state->clock.source()}); return 0;
    });
    service->add_cmd(20, "GetDeviceLocationNameOperationEventReadableHandle", [state](Request&, Response& rsp) {
        return event_handle(state->zone_event, rsp);
    });
    for (const auto command : {100u, 101u}) {
        service->add_cmd(command, "ToCalendarTime", [state, command](Request& req, Response& rsp) -> uint32_t {
            if (!arguments(req, 8)) return out_of_range;
            std::array<uint8_t, tz::kRuleSize> rule{};
            const uint8_t* bytes = state->utc.data();
            if (command == 100) { const auto result = input_rule(req, rule); if (result) return result; bytes = rule.data(); }
            tz::CalendarTime calendar{}; tz::CalendarAdditionalInfo info{};
            if (!tz::to_calendar(bytes, req.arg<int64_t>(0), &calendar, &info)) return conversion_failed;
            rsp.push(calendar); rsp.push(info); return 0;
        });
    }
    for (const auto command : {201u, 202u}) {
        service->add_cmd(command, "ToPosixTime", [state, command](Request& req, Response& rsp) -> uint32_t {
            if (!arguments(req, sizeof(tz::CalendarTime))) return out_of_range;
            std::array<uint8_t, tz::kRuleSize> rule{};
            const uint8_t* bytes = state->utc.data();
            if (command == 201) { const auto result = input_rule(req, rule); if (result) return result; bytes = rule.data(); }
            int64_t times[2]{};
            const int count = tz::to_posix(bytes, req.arg<tz::CalendarTime>(0), times);
            const auto result = output(req.out_buf(0), times, static_cast<size_t>(count) * sizeof(int64_t));
            if (result) return result;
            rsp.push<int32_t>(count); return 0;
        });
    }
    return service;
}
Ptr static_service() {
    const auto state = current();
    auto service = std::make_shared<TimeService>("time:u");
    for (const auto command : {0u, 1u, 4u})
        service->add_cmd(command, "GetSystemClock", [state](Request&, Response& rsp) -> uint32_t {
            if (!state->ready) return kernel::kInvalidState;
            rsp.push_object(system_clock(state)); return 0;
        });
    service->add_cmd(2, "GetStandardSteadyClock", [state](Request&, Response& rsp) -> uint32_t {
        if (!state->ready) return kernel::kInvalidState;
        rsp.push_object(steady_clock(state)); return 0;
    });
    service->add_cmd(3, "GetTimeZoneService", [state](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(zone(state)); return 0;
    });
    service->add_cmd(20, "GetSharedMemoryNativeHandle", [state](Request&, Response& rsp) -> uint32_t {
        std::lock_guard lock(state->mutex);
        if (!state->ready) return kernel::kInvalidState;
        if (!state->shared) {
            auto shared = kernel::shared_memory_create(clock::shared_size, "time", 1);
            if (!shared) return kernel::kOutOfMemory;
            if (!state->clock.shared_image(reinterpret_cast<void*>(shared->host), shared->size)) return kernel::kInvalidState;
            state->shared = std::move(shared);
        }
        const auto handle = kernel::handle_add(state->shared);
        if (!handle) return kernel::kOutOfHandles;
        rsp.out_copy.push_back(handle); return 0;
    });
    service->add_cmd(50, "SetStandardSteadyClockInternalOffset", denied);
    service->add_cmd(100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0); return 0;
    });
    service->add_cmd(101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", denied);
    return service;
}
} // namespace
void register_time() { add("time:u", static_service); }
void time_reset() { std::lock_guard lock(state_mutex); state.reset(); }
} // namespace nxrt::services
