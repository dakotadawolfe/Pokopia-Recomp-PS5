// SPDX-License-Identifier: GPL-3.0-or-later
#include "time_clock.h"
#include <cstring>
#include <limits>

namespace nxrt::services::time_clock {
namespace {
constexpr uint64_t billion = 1000000000;
constexpr int64_t epoch_ns = steady_epoch * billion;
}
bool ticks_to_ns(uint64_t ticks, uint64_t frequency, uint64_t* result) {
    if (!frequency || !result) return false;
    const auto whole = ticks / frequency, fraction = ticks % frequency;
    if (whole > UINT64_MAX / billion || fraction > UINT64_MAX / billion) return false;
    const auto ns = whole * billion, part = fraction * billion / frequency;
    if (part > UINT64_MAX - ns) return false;
    *result = ns + part; return true;
}
bool Clock::initialize(uint64_t ticks, uint64_t frequency, int64_t utc, const SourceId& source) {
    ready_ = false;
    uint64_t ns = 0;
    if (!ticks_to_ns(ticks, frequency, &ns) || ns > INT64_MAX || utc < INT64_MIN + steady_epoch) return false;
    origin_ns_ = ns; frequency_ = frequency; utc_offset_ = utc - steady_epoch; source_ = source; ready_ = true;
    return true;
}
bool Clock::sample(uint64_t ticks, TimePoint* steady, int64_t* utc) const {
    uint64_t ns = 0;
    if (!ready_ || !ticks_to_ns(ticks, frequency_, &ns) || ns < origin_ns_ || ns > INT64_MAX) return false;
    const auto elapsed = ns - origin_ns_;
    if (elapsed > static_cast<uint64_t>(INT64_MAX - epoch_ns)) return false;
    const auto seconds = static_cast<int64_t>((elapsed + epoch_ns) / billion);
    if (utc_offset_ > INT64_MAX - seconds) return false;
    if (steady) *steady = TimePoint{seconds, source_};
    if (utc) *utc = utc_offset_ + seconds;
    return true;
}
bool Clock::context(uint64_t ticks, SystemContext* out) const {
    if (!out) return false;
    TimePoint point{};
    if (!sample(ticks, &point, nullptr)) return false;
    *out = {utc_offset_, point}; return true;
}
bool Clock::shared_image(void* out, size_t capacity) const {
    if (!ready_ || !out || capacity < shared_size) return false;
    auto* bytes = static_cast<uint8_t*>(out);
    std::memset(bytes, 0, shared_size);
    // This image is published once before the guest receives its handle. Slot
    // one and counter one match the guest's double-buffered read protocol.
    const auto block = [bytes](size_t offset, size_t padding, const auto& value) {
        const uint32_t index = 1;
        std::memcpy(bytes + offset + 4 + padding + sizeof value, &value, sizeof value);
        std::memcpy(bytes + offset, &index, sizeof index);
    };
    const SteadyContext steady{epoch_ns - static_cast<int64_t>(origin_ns_), source_};
    const SystemContext context{utc_offset_, {steady_epoch, source_}};
    block(0x000, 4, steady);
    block(0x038, 4, context);
    block(0x080, 4, context);
    block(0x0c8, 0, uint8_t{0});
    // Newer SDKs derive steady time from this block instead of 0x00. These
    // offsets are nanoseconds, not a SystemClockContext's seconds. A zero rate
    // and equal initial/target offsets describe our unadjusted steady clock.
    struct Adjust {
        uint64_t clock_offset;
        int64_t multiplier, divisor_log2, initial_offset_ns, target_offset_ns;
        SourceId source;
    };
    static_assert(sizeof(Adjust) == 0x38);
    const Adjust adjust{origin_ns_, 0, 0, steady.base_ns, steady.base_ns, source_};
    block(0x0d0, 4, adjust);
    return true;
}
} // namespace nxrt::services::time_clock
