// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Horizon clock model, independent of host memory and IPC ownership.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace nxrt::services::time_clock {
using SourceId = std::array<uint8_t, 16>;
struct TimePoint { int64_t seconds; SourceId source; };
struct SystemContext { int64_t offset; TimePoint point; };
struct SteadyContext { int64_t base_ns; SourceId source; };
static_assert(sizeof(TimePoint) == 0x18 && sizeof(SystemContext) == 0x20 && sizeof(SteadyContext) == 0x18);
constexpr int64_t steady_epoch = 3600;
constexpr size_t shared_size = 0x1000;

bool ticks_to_ns(uint64_t ticks, uint64_t frequency, uint64_t* result);

class Clock {
public:
    bool initialize(uint64_t ticks, uint64_t frequency, int64_t utc_seconds, const SourceId& source);
    bool sample(uint64_t ticks, TimePoint* steady, int64_t* utc) const;
    bool context(uint64_t ticks, SystemContext* out) const;
    bool shared_image(void* out, size_t capacity) const;
    const SourceId& source() const { return source_; }
private:
    bool ready_ = false;
    uint64_t frequency_ = 0, origin_ns_ = 0;
    int64_t utc_offset_ = 0;
    SourceId source_{};
};
} // namespace nxrt::services::time_clock
