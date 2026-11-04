// SPDX-License-Identifier: GPL-3.0-or-later
// Optional bounded native trace. File writes are synchronous and serialized;
// leave tracing off when measuring renderer throughput.
#include "nxrt/ftrace.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxrt::ft {
std::atomic<bool> g_on{false};
namespace {
std::mutex mutex;
FILE* output = nullptr;
std::vector<std::string> names;
uint64_t events = 0, capacity = 0;
}
uint64_t now_ns() { return FrameLimiter::now_ns(); }
uint16_t zone_id(const char* name) {
    std::lock_guard lock(mutex);
    const auto found = std::find(names.begin(), names.end(), name);
    if (found != names.end()) return static_cast<uint16_t>(found - names.begin());
    if (names.size() >= UINT16_MAX) return UINT16_MAX;
    const auto id = static_cast<uint16_t>(names.size());
    names.emplace_back(name);
    if (output) std::fprintf(output, "# zone %u %s\n", id, name);
    return id;
}
void event(uint16_t zone, uint64_t t0, uint64_t t1, uint32_t arg) {
    if (!on()) return;
    std::lock_guard lock(mutex);
    if (!output || events >= capacity) return;
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::fprintf(output, "%llu %llu %zu %u %u\n", static_cast<unsigned long long>(t0),
        static_cast<unsigned long long>(t1 >= t0 ? t1 - t0 : 0), tid, zone, arg);
    if (++events == capacity) {
        std::fprintf(output, "# capacity reached: %llu events\n", static_cast<unsigned long long>(events));
        std::fflush(output);
        g_on.store(false);
    }
}
void mark(const char* name, uint32_t arg) {
    if (!on()) return;
    const auto id = zone_id(name);
    const auto at = now_ns();
    event(id, at, at, arg);
}
void frame() { mark("frame"); }
void start_from_config() {
    const std::string path = config_get_str("prof", "trace", "");
    if (path.empty()) return;
    if (config_get_int("prof", "stall_ms", 0) != 0) {
        NXLOG_E("prof", "PS5 thread-stack sampling is unavailable; native frame trace was not started");
        return;
    }
    std::lock_guard lock(mutex);
    if (output) return;
    output = std::fopen(path.c_str(), "wb");
    if (!output) { NXLOG_E("prof", "cannot open trace %s", path.c_str()); return; }
    events = 0;
    capacity = static_cast<uint64_t>(std::clamp<int64_t>(config_get_int("prof", "trace_events", 100000), 1, 6000000));
    for (size_t i = 0; i < names.size(); ++i) std::fprintf(output, "# zone %zu %s\n", i, names[i].c_str());
    g_on.store(true);
}
void stop() {
    g_on.store(false);
    std::lock_guard lock(mutex);
    if (output) std::fclose(output);
    output = nullptr;
}
} // namespace nxrt::ft
