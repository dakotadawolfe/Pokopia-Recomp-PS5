// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/gauge.h"
#include <map>
#include <mutex>

namespace nxrt {
namespace {
struct Registry {
    std::mutex m;
    std::map<std::string, std::function<int64_t()>> g;
};
Registry& reg() {
    static Registry* r = new Registry;     // never destroyed: counters in other statics may unregister at exit
    return *r;
}

} // namespace

void gauge_register(const char* name, std::function<int64_t()> fn) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g[name] = std::move(fn);
}

void gauge_unregister(const char* name) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g.erase(name);
}

std::vector<std::pair<std::string, int64_t>> gauge_read_all() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    std::vector<std::pair<std::string, int64_t>> out;
    out.reserve(r.g.size());
    for (auto& kv : r.g) out.emplace_back(kv.first, kv.second());
    return out;
}

GaugeCounter::GaugeCounter(const char* name) : name_(name) {
    gauge_register(name, [this] { return get(); });
}
GaugeCounter::~GaugeCounter() { gauge_unregister(name_.c_str()); }

} // namespace nxrt
