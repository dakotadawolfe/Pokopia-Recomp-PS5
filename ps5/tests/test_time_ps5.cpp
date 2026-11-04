// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "time_clock.h"
#include "timezone.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
namespace nxrt::services { void register_time(); void time_reset(); }
NXTEST_GROUP(time_ps5)
namespace {
namespace ipc = nxrt::ipc; namespace k = nxrt::kernel; namespace mem = nxrt::mem;
namespace tc = nxrt::services::time_clock; namespace tz = nxrt::services::tz;
using Ptr = std::shared_ptr<ipc::Service>;
struct Answer { uint32_t result; ipc::Response response; };
Answer call(const Ptr& service, uint32_t command) {
    ipc::Request req; req.cmd = command; Answer answer{};
    answer.result = service->handle(req, answer.response); return answer;
}
Ptr child(const Ptr& service, uint32_t command) {
    auto answer = call(service, command);
    return !answer.result && answer.response.out_objects.size() == 1 ? answer.response.out_objects[0] : nullptr;
}
template<class T> T value(const Answer& answer) {
    if (answer.result || answer.response.out.size() < sizeof(T)) throw std::runtime_error("missing time reply");
    T result{}; std::memcpy(&result, answer.response.out.data(), sizeof result); return result;
}
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    Ptr service; uint64_t data = 0;
    Fixture() {
        nxrt::services::time_reset(); k::shared_memory_reset_for_tests();
        k::kernel_reset(); ipc::registry_reset(); mem::shutdown();
        nxrt::game::set_current(&game);
        if (!mem::init({.heap_reserve = 64ull << 20}) || !(data = mem::alloc_kernel(0x8000)))
            throw std::runtime_error("cannot allocate time test memory");
        nxrt::services::register_time(); service = ipc::make_service("time:u");
    }
    ~Fixture() {
        service.reset(); nxrt::services::time_reset(); ipc::registry_reset();
        k::shared_memory_reset_for_tests(); k::kernel_reset(); mem::shutdown();
        nxrt::game::set_current(previous);
    }
};
uint32_t shared_handle(const Ptr& service) {
    const auto answer = call(service, 20);
    return !answer.result && answer.response.out_copy.size() == 1 ? answer.response.out_copy[0] : 0;
}
template<class T> T shared_value(uint64_t base, size_t offset) {
    uint32_t index; std::memcpy(&index, reinterpret_cast<const void*>(base + offset), 4);
    T result{}; std::memcpy(&result, reinterpret_cast<const void*>(base + offset + 8 + (index & 1) * sizeof(T)), sizeof(T));
    return result;
}
}

NXTEST(time_user_clocks_are_backed_by_host_time_and_monotonic_switch2_ticks) {
    Fixture f; CHECK(!f.service->is_stub()); CHECK_EQ(k::tick_hz(), 31250000ull);
    for (const auto name : {"time:a", "time:s", "time:r", "time:su"}) CHECK(!ipc::has_service(name));
    const auto steady = child(f.service, 2); REQUIRE(steady);
    const auto before = value<tc::TimePoint>(call(steady, 0));
    for (auto command : {0u, 1u, 4u}) {
        const auto clock = child(f.service, command); REQUIRE(clock);
        const auto time = value<int64_t>(call(clock, 0));
        const auto host = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        CHECK(time >= host - 2 && time <= host + 2);
        const auto context = value<tc::SystemContext>(call(clock, 2));
        CHECK(context.point.source == before.source);
        CHECK(context.offset + context.point.seconds >= time - 1 && context.offset + context.point.seconds <= time + 1);
    }
    const auto after = value<tc::TimePoint>(call(steady, 0));
    CHECK(after.seconds >= before.seconds); CHECK(after.source == before.source);
}

NXTEST(time_shared_handle_maps_real_read_only_clock_data_and_matches_ipc) {
    Fixture f; const auto first = shared_handle(f.service), second = shared_handle(f.service); REQUIRE(first && second);
    const auto object = k::handle_as<k::KSharedMemory>(first); REQUIRE(object);
    CHECK(k::handle_as<k::KSharedMemory>(second) == object); CHECK_EQ(object->size, 0x1000ull);
    constexpr uint64_t view = mem::kAliasBase + 0x100000;
    CHECK_EQ(k::shared_memory_map(first, view, 0x1000, 3), k::kInvalidPermission);
    REQUIRE(k::shared_memory_map(first, view, 0x1000, 1) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(view), reinterpret_cast<void*>(object->host), 0x1000) == 0);
    k::handle_close(first);
    const auto steady = shared_value<tc::SteadyContext>(view, 0x00);
    uint64_t ns = 0; REQUIRE(tc::ticks_to_ns(k::system_tick(), k::tick_hz(), &ns));
    const auto fast_steady = (steady.base_ns + static_cast<int64_t>(ns)) / 1000000000;
    struct Adjustment {
        int64_t clock_offset, multiplier, divisor_log2, initial_offset_ns, target_offset_ns;
        tc::SourceId source;
    };
    const auto adjustment = shared_value<Adjustment>(view, 0xd0);
    REQUIRE(adjustment.multiplier == 0 && adjustment.divisor_log2 == 0);
    const int64_t candidate = (((static_cast<int64_t>(ns) - adjustment.clock_offset) * adjustment.multiplier) >> adjustment.divisor_log2) + adjustment.initial_offset_ns;
    const auto correction = adjustment.multiplier < 0 ? std::max(candidate, adjustment.target_offset_ns) : std::min(candidate, adjustment.target_offset_ns);
    CHECK_EQ((static_cast<int64_t>(ns) + correction) / 1000000000, fast_steady);
    CHECK(adjustment.source == steady.source);
    const auto ipc_steady = value<tc::TimePoint>(call(child(f.service, 2), 0));
    CHECK(ipc_steady.seconds >= fast_steady && ipc_steady.seconds <= fast_steady + 1);
    CHECK(ipc_steady.source == steady.source);
    for (size_t offset : {0x38u, 0x80u}) {
        const auto context = shared_value<tc::SystemContext>(view, offset);
        const auto ipc_time = value<int64_t>(call(child(f.service, offset == 0x38 ? 0 : 1), 0));
        CHECK(context.point.source == steady.source);
        CHECK(ipc_time >= context.offset + fast_steady && ipc_time <= context.offset + fast_steady + 1);
    }
    CHECK_EQ(k::shared_memory_unmap(second, view, 0x1000), 0u);
    k::handle_close(second);
}

NXTEST(time_reset_isolates_sources_events_and_existing_backing) {
    Fixture f; const auto old_clock = child(f.service, 0); REQUIRE(old_clock);
    const auto old_context = value<tc::SystemContext>(call(old_clock, 2));
    const auto old_shared = shared_handle(f.service); REQUIRE(old_shared);
    const auto old_object = k::handle_as<k::KSharedMemory>(old_shared); REQUIRE(old_object);
    const auto first = call(old_clock, 4), second = call(old_clock, 4);
    REQUIRE(first.response.out_copy.size() == 1 && second.response.out_copy.size() == 1);
    const auto event = k::handle_as<k::KEvent>(first.response.out_copy[0]); REQUIRE(event);
    CHECK(k::handle_as<k::KEvent>(second.response.out_copy[0])->state == event->state); CHECK(!event->signaled());
    nxrt::services::time_reset(); const auto fresh = ipc::make_service("time:u");
    const auto fresh_clock = child(fresh, 0); REQUIRE(fresh_clock);
    const auto next = value<tc::SystemContext>(call(fresh_clock, 2)); CHECK(next.point.source != old_context.point.source);
    CHECK(value<tc::SystemContext>(call(old_clock, 2)).point.source == old_context.point.source);
    CHECK(shared_value<tc::SteadyContext>(old_object->host, 0).source == old_context.point.source);
    const auto next_event = call(fresh_clock, 4); REQUIRE(next_event.response.out_copy.size() == 1);
    CHECK(k::handle_as<k::KEvent>(next_event.response.out_copy[0])->state != event->state);
}

NXTEST(time_user_writes_and_unimplemented_operations_fail_explicitly) {
    Fixture f; const auto clock = child(f.service, 0), steady = child(f.service, 2), zone = child(f.service, 3);
    REQUIRE(clock && steady && zone);
    for (auto command : {1u, 3u}) CHECK_EQ(call(clock, command).result, 0x274u);
    CHECK_EQ(call(steady, 3).result, 0x274u); CHECK_EQ(call(zone, 1).result, 0x274u);
    for (auto command : {50u, 101u}) CHECK_EQ(call(f.service, command).result, 0x274u);
    for (const auto& service : {f.service, clock, steady, zone}) CHECK_EQ(call(service, 999).result, k::kNotImplemented);
    CHECK_EQ(call(f.service, 400).result, k::kNotImplemented);
    CHECK_EQ(call(steady, 101).result, k::kNotImplemented);
    CHECK_EQ(call(zone, 5).result, k::kNotImplemented);
}

NXTEST(time_zone_utc_rule_and_calendar_service_round_trip) {
    Fixture f; const auto zone = child(f.service, 3); REQUIRE(zone);
    const auto name = value<std::array<char, 0x24>>(call(zone, 0)); CHECK(std::memcmp(name.data(), "UTC\0", 4) == 0);
    CHECK_EQ(value<int32_t>(call(zone, 2)), 1);
    ipc::Request req; ipc::Response rsp; req.cmd = 4;
    req.in = reinterpret_cast<const uint8_t*>(name.data()); req.in_size = name.size(); req.b.push_back({f.data, 0x4000});
    REQUIRE(zone->handle(req, rsp) == 0); CHECK(tz::rule_valid(reinterpret_cast<uint8_t*>(f.data)));
    const int64_t timestamp = 1700000000; req = {}; rsp = {}; req.cmd = 100;
    req.in = reinterpret_cast<const uint8_t*>(&timestamp); req.in_size = 8; req.a.push_back({f.data, 0x4000});
    REQUIRE(zone->handle(req, rsp) == 0); REQUIRE(rsp.out.size() == 0x20);
    tz::CalendarTime calendar{}; std::memcpy(&calendar, rsp.out.data(), sizeof calendar);
    CHECK_EQ(calendar.year, 2023); CHECK_EQ(calendar.month, 11); CHECK_EQ(calendar.day, 14);
    CHECK_EQ(calendar.hour, 22); CHECK_EQ(calendar.minute, 13); CHECK_EQ(calendar.second, 20);
    req = {}; rsp = {}; req.cmd = 201; req.in = reinterpret_cast<const uint8_t*>(&calendar); req.in_size = sizeof calendar;
    req.a.push_back({f.data, 0x4000}); req.c.push_back({f.data + 0x4000, 16});
    REQUIRE(zone->handle(req, rsp) == 0); REQUIRE(rsp.out.size() == 4);
    CHECK_EQ(*reinterpret_cast<int64_t*>(f.data + 0x4000), timestamp);
}

NXTEST(time_zone_buffers_are_bounded_and_guest_permissions_are_checked) {
    Fixture f; const auto zone = child(f.service, 3); REQUIRE(zone);
    const std::array<char, 0x24> name{'U', 'T', 'C'};
    ipc::Request req; ipc::Response rsp; req.cmd = 4;
    req.in = reinterpret_cast<const uint8_t*>(name.data()); req.in_size = name.size(); req.b.push_back({f.data, 0x3fff});
    std::memset(reinterpret_cast<void*>(f.data), 0xa5, 0x4000);
    CHECK_EQ(zone->handle(req, rsp), 0x70c74u); CHECK_EQ(*reinterpret_cast<uint64_t*>(f.data), 0xa5a5a5a5a5a5a5a5ull);
    req.b[0].size = 0x4000; REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::R));
    CHECK_EQ(zone->handle(req, rsp), k::kInvalidMemoryState);
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::RW)); REQUIRE(zone->handle(req, rsp) == 0);
    const int64_t timestamp = 1700000000; req = {}; req.cmd = 100;
    req.in = reinterpret_cast<const uint8_t*>(&timestamp); req.in_size = 8; req.a.push_back({f.data, 0x4000});
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::None)); CHECK_EQ(zone->handle(req, rsp), k::kInvalidMemoryState);
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::RW));
    *reinterpret_cast<int32_t*>(f.data + 4) = 0; CHECK_EQ(zone->handle(req, rsp), 0x70e74u);
    req = {}; req.cmd = 101; CHECK_EQ(zone->handle(req, rsp), 0x70c74u);
    for (const int64_t extreme : {INT64_MIN, INT64_MAX}) {
        req.in = reinterpret_cast<const uint8_t*>(&extreme); req.in_size = 8;
        CHECK_EQ(zone->handle(req, rsp), 0x70e74u);
    }
}
