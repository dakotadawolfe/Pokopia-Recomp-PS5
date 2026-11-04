// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "timezone.h"
#include <array>
#include <cstring>
#include <limits>
NXTEST_GROUP(time_zone)
namespace tz = nxrt::services::tz;
namespace {
using Rule = std::array<uint8_t, tz::kRuleSize>;
template<class T> void put(Rule& rule, size_t at, T value) { std::memcpy(rule.data() + at, &value, sizeof value); }
Rule berlin() {
    Rule rule{};
    put<int32_t>(rule, 0, 2); put<int32_t>(rule, 4, 2); put<int32_t>(rule, 8, 9);
    put<int64_t>(rule, 16, 1679792400); put<int64_t>(rule, 24, 1698541200);
    rule[8016] = 1; rule[8017] = 0;
    put<int32_t>(rule, 9016, 3600); put<int32_t>(rule, 9032, 7200);
    rule[9036] = 1; put<int32_t>(rule, 9040, 4);
    std::memcpy(rule.data() + 11064, "CET\0CEST", 9);
    return rule;
}
}
NXTEST(time_zone_utc_gregorian_oracles_and_round_trip) {
    Rule rule{}; tz::make_utc_rule(rule.data()); REQUIRE(tz::rule_valid(rule.data()));
    struct Case { int64_t seconds; int year, month, day, hour, minute, second, weekday; };
    for (const auto& c : {Case{0, 1970, 1, 1, 0, 0, 0, 4}, {-1, 1969, 12, 31, 23, 59, 59, 3},
                         {951782400, 2000, 2, 29, 0, 0, 0, 2}, {1700000000, 2023, 11, 14, 22, 13, 20, 2},
                         {4102444800, 2100, 1, 1, 0, 0, 0, 5}}) {
        tz::CalendarTime calendar{}; tz::CalendarAdditionalInfo info{};
        REQUIRE(tz::to_calendar(rule.data(), c.seconds, &calendar, &info));
        CHECK_EQ(calendar.year, c.year); CHECK_EQ(calendar.month, c.month); CHECK_EQ(calendar.day, c.day);
        CHECK_EQ(calendar.hour, c.hour); CHECK_EQ(calendar.minute, c.minute); CHECK_EQ(calendar.second, c.second);
        CHECK_EQ(info.day_of_week, static_cast<uint32_t>(c.weekday)); CHECK_EQ(info.utc_offset, 0);
        CHECK(std::memcmp(info.tz_name, "UTC\0", 4) == 0);
        int64_t back[2]{}; REQUIRE(tz::to_posix(rule.data(), calendar, back) == 1); CHECK_EQ(back[0], c.seconds);
    }
    for (int16_t year : {INT16_MIN, INT16_MAX}) {
        const tz::CalendarTime calendar{year, 1, 1, 0, 0, 0, 0};
        int64_t time[2]{}; REQUIRE(tz::to_posix(rule.data(), calendar, time) == 1);
        tz::CalendarTime round_trip{}; tz::CalendarAdditionalInfo info{};
        REQUIRE(tz::to_calendar(rule.data(), time[0], &round_trip, &info));
        CHECK_EQ(round_trip.year, year); CHECK_EQ(round_trip.month, 1); CHECK_EQ(round_trip.day, 1);
    }
}
NXTEST(time_zone_dst_gap_fold_and_supplied_rule_are_preserved) {
    const auto rule = berlin(); REQUIRE(tz::rule_valid(rule.data()));
    tz::CalendarTime cal{2023, 10, 29, 2, 30, 0, 0}; int64_t times[2]{};
    CHECK_EQ(tz::to_posix(rule.data(), cal, times), 2);
    CHECK_EQ(times[0], 1698539400ll); CHECK_EQ(times[1], 1698543000ll);
    tz::CalendarAdditionalInfo info{};
    REQUIRE(tz::to_calendar(rule.data(), times[0], &cal, &info)); CHECK_EQ(info.is_dst, 1); CHECK_EQ(info.utc_offset, 7200);
    REQUIRE(tz::to_calendar(rule.data(), times[1], &cal, &info)); CHECK_EQ(info.is_dst, 0); CHECK_EQ(info.utc_offset, 3600);
    cal = {2023, 3, 26, 2, 30, 0, 0}; CHECK_EQ(tz::to_posix(rule.data(), cal, times), 0);
}
NXTEST(time_zone_invalid_rules_and_remote_timestamps_are_rejected) {
    auto rule = berlin(); put<int32_t>(rule, 4, 0); CHECK(!tz::rule_valid(rule.data()));
    rule = berlin(); rule[8016] = 12; CHECK(!tz::rule_valid(rule.data()));
    rule = berlin(); put<int64_t>(rule, 16, INT64_MIN); CHECK(!tz::rule_valid(rule.data()));
    rule = berlin(); tz::CalendarTime cal{}; tz::CalendarAdditionalInfo info{};
    CHECK(!tz::to_calendar(rule.data(), INT64_MAX, &cal, &info));
    CHECK(!tz::to_calendar(rule.data(), INT64_MIN, &cal, &info));
}
