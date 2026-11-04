// SPDX-License-Identifier: GPL-3.0-or-later
// Renderer diagnostics without the Windows stack sampler or desktop overlay.
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

namespace nxrt {
namespace {
struct State {
    std::mutex mu;
    std::map<size_t, ThreadRecord> threads;
    size_t next = 1;
    std::vector<std::string (*)()> reports;
    std::atomic<uint64_t> frames{0}, progress_ns{0}, game_presents{0};
    std::atomic<uint64_t> phases[kHpCount]{};
    std::atomic<void (*)()> listener{nullptr};
};
State& state() { static auto* s = new State; return *s; }
thread_local size_t thread_record = 0;
thread_local Ctx* thread_ctx = nullptr;
thread_local bool render_thread = false, presenting_thread = false;
}
uint64_t hitch_ticks() { return FrameLimiter::now_ns(); }
void hitch_phase_add(int phase, uint64_t ns) {
    if (phase >= 0 && phase < kHpCount) state().phases[phase].fetch_add(ns, std::memory_order_relaxed);
}
void hitch_set_render_thread() { render_thread = true; }
bool hitch_on_render_thread() { return render_thread; }
void hitch_note_game_present() { presenting_thread = true; state().game_presents.fetch_add(1, std::memory_order_relaxed); }
bool hitch_on_game_present_thread() { return presenting_thread; }
void hitch_add_counter(const char* name, uint64_t (*fn)()) {
    if (name && fn) gauge_register((std::string("renderer.counter.") + name).c_str(), [fn] { return static_cast<int64_t>(fn()); });
}
void hitch_add_level(const char* name, int64_t (*fn)()) {
    if (name && fn) gauge_register((std::string("renderer.level.") + name).c_str(), fn);
}
size_t diag_register_thread(const char* name, Ctx* ctx, uint64_t guest_tid) {
    auto& s = state(); std::lock_guard lock(s.mu);
    if (!thread_record) thread_record = s.next++;
    // These IDs identify diagnostic records, not suspendable OS threads.
    s.threads[thread_record] = {name ? name : "", static_cast<uint32_t>(thread_record), nullptr, ctx, guest_tid};
    thread_ctx = ctx; return thread_record;
}
void diag_unregister_thread() {
    auto& s = state(); std::lock_guard lock(s.mu);
    s.threads.erase(thread_record); thread_record = 0; thread_ctx = nullptr;
}
void diag_set_thread_ctx(Ctx* ctx) {
    thread_ctx = ctx; auto& s = state(); std::lock_guard lock(s.mu);
    if (thread_record) s.threads[thread_record].ctx = ctx;
}
Ctx* diag_thread_ctx() { return thread_ctx; }
std::vector<ThreadRecord> diag_threads() {
    auto& s = state(); std::lock_guard lock(s.mu);
    std::vector<ThreadRecord> result; for (auto& [id, r] : s.threads) result.push_back(r);
    return result;
}
void diag_progress() { state().progress_ns.store(hitch_ticks(), std::memory_order_relaxed); }
void diag_frame_tick_nolisten() { diag_progress(); state().frames.fetch_add(1, std::memory_order_relaxed); }
void diag_frame_tick() {
    diag_frame_tick_nolisten(); if (auto fn = state().listener.load(std::memory_order_acquire)) fn();
}
void diag_set_frame_listener(void (*fn)()) { state().listener.store(fn, std::memory_order_release); }
uint64_t diag_frame_count() { return state().frames.load(std::memory_order_relaxed); }
void diag_add_report(std::string (*fn)()) {
    auto& s = state(); std::lock_guard lock(s.mu);
    if (fn && std::find(s.reports.begin(), s.reports.end(), fn) == s.reports.end()) s.reports.push_back(fn);
}
std::string diag_thread_dump() {
    auto& s = state(); std::vector<std::string (*)()> reports;
    { std::lock_guard lock(s.mu); reports = s.reports; }
    std::ostringstream out;
    out << "PS5 renderer diagnostics; OS stack sampling unavailable\nframes=" << diag_frame_count()
        << " game_presents=" << s.game_presents.load() << " progress_ns=" << s.progress_ns.load() << '\n';
    for (auto& r : diag_threads()) out << "thread=" << r.name << " record=" << r.host_tid << " guest=" << r.guest_tid << '\n';
    for (int n = 0; n < kHpCount; ++n) out << "phase_" << n << "_ns=" << s.phases[n].load() << '\n';
    for (auto fn : reports) out << fn() << '\n';
    return out.str();
}
namespace gfx {
namespace { std::atomic<int> backlog{0}; }
void set_shader_backlog(int n) { backlog.store(std::max(n, 0), std::memory_order_relaxed); }
int shader_backlog() { return backlog.load(std::memory_order_relaxed); }
}
}
