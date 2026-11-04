// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "time_clock.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
NXTEST_GROUP(time_clock)
namespace tc = nxrt::services::time_clock;
namespace {
struct Adjustment {
    int64_t clock_offset, multiplier, divisor_log2, initial_offset_ns, target_offset_ns;
    tc::SourceId source;
};
static_assert(sizeof(Adjustment) == 0x38);
int64_t adjusted_steady(const Adjustment& a, int64_t ns) {
    const int64_t candidate = (((ns - a.clock_offset) * a.multiplier) >> a.divisor_log2) + a.initial_offset_ns;
    const auto adjustment = a.multiplier < 0 ? std::max(candidate, a.target_offset_ns) : std::min(candidate, a.target_offset_ns);
    return (ns + adjustment) / 1000000000;
}
template<class T> T live(const std::array<uint8_t, tc::shared_size>& bytes, size_t offset, size_t pad) {
    uint32_t index; std::memcpy(&index, bytes.data() + offset, 4);
    T value{}; std::memcpy(&value, bytes.data() + offset + 4 + pad + (index & 1) * sizeof(T), sizeof(T));
    return value;
}
}
NXTEST(time_clock_frequencies_preserve_fractional_epoch_and_ipc_shared_agreement) {
    for (uint64_t hz : {19200000ull, 31250000ull}) for (uint64_t origin_seconds : {7ull, 7200ull}) {
        tc::Clock clock; const uint64_t origin = hz * origin_seconds + hz / 2;
        const tc::SourceId source{1, 2, 3, 4};
        REQUIRE(clock.initialize(origin, hz, 1700000000, source));
        std::array<uint8_t, tc::shared_size> bytes{}; REQUIRE(clock.shared_image(bytes.data(), bytes.size()));
        const auto steady = live<tc::SteadyContext>(bytes, 0x00, 4);
        CHECK_EQ(steady.base_ns, (3600 - static_cast<int64_t>(origin_seconds)) * 1000000000 - 500000000);
        CHECK(steady.source == source);
        const auto adjustment = live<Adjustment>(bytes, 0xd0, 4);
        CHECK(adjustment.source == source);
        REQUIRE(adjustment.multiplier == 0 && adjustment.divisor_log2 == 0);
        for (uint64_t elapsed : {uint64_t{0}, uint64_t{1}, hz / 2, hz, hz * 25 + hz / 4}) {
            tc::TimePoint point{}; int64_t utc = 0;
            REQUIRE(clock.sample(origin + elapsed, &point, &utc));
            CHECK_EQ(point.seconds, static_cast<int64_t>(3600 + elapsed / hz));
            CHECK_EQ(utc, static_cast<int64_t>(1700000000 + elapsed / hz));
            const auto raw_ns = static_cast<int64_t>((origin + elapsed) / hz * 1000000000 + (origin + elapsed) % hz * 1000000000 / hz);
            const auto fast_steady = (steady.base_ns + raw_ns) / 1000000000;
            CHECK_EQ(fast_steady, point.seconds);
            CHECK_EQ(adjusted_steady(adjustment, raw_ns), point.seconds);
            for (size_t offset : {0x38u, 0x80u}) {
                const auto context = live<tc::SystemContext>(bytes, offset, 4);
                CHECK(context.point.source == source);
                CHECK_EQ(context.offset + fast_steady, utc);
                CHECK_EQ(context.offset + adjusted_steady(adjustment, raw_ns), utc);
            }
            tc::SystemContext context{}; REQUIRE(clock.context(origin + elapsed, &context));
            CHECK_EQ(context.offset + context.point.seconds, utc);
        }
        CHECK_EQ(live<uint8_t>(bytes, 0xc8, 0), 0);
    }
}
NXTEST(time_clock_rejects_invalid_origins_backward_ticks_and_overflow) {
    uint64_t ns = 0; CHECK(!tc::ticks_to_ns(1, 0, &ns)); CHECK(!tc::ticks_to_ns(UINT64_MAX, 1, &ns));
    CHECK(!tc::ticks_to_ns(1, 19200000, nullptr));
    tc::Clock clock; tc::SourceId source{}; tc::TimePoint point{};
    CHECK(!clock.sample(0, &point, nullptr));
    CHECK(!clock.initialize(0, 19200000, INT64_MIN, source));
    CHECK(!clock.initialize(UINT64_MAX, 1, 0, source));
    REQUIRE(clock.initialize(19200000, 19200000, 1700000000, source));
    CHECK(!clock.sample(19199999, &point, nullptr));
    REQUIRE(clock.initialize(0, 19200000, INT64_MAX, source));
    CHECK(!clock.sample(19200000, &point, nullptr));
}
NXTEST(time_clock_image_bounds_and_source_replacement) {
    tc::Clock clock; std::array<uint8_t, tc::shared_size + 16> bytes; bytes.fill(0xa5);
    const tc::SourceId first{1}, second{2};
    CHECK(!clock.shared_image(bytes.data(), bytes.size()));
    REQUIRE(clock.initialize(0, 31250000, 1700000000, first));
    CHECK(!clock.shared_image(bytes.data(), tc::shared_size - 1));
    CHECK(!clock.shared_image(nullptr, tc::shared_size));
    REQUIRE(clock.shared_image(bytes.data(), bytes.size()));
    for (size_t n = tc::shared_size; n < bytes.size(); ++n) CHECK_EQ(bytes[n], 0xa5);
    REQUIRE(clock.initialize(31250000, 31250000, 1800000000, second));
    tc::TimePoint point{}; int64_t utc = 0; REQUIRE(clock.sample(31250000, &point, &utc));
    CHECK(point.source == second); CHECK_EQ(point.seconds, 3600); CHECK_EQ(utc, 1800000000);
}
