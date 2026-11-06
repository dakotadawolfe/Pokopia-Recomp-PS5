// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 texture dirty tracking uses exact page snapshots. It never changes CPU
// permissions and cannot implement deferred GPU read guards. Pool mirrors are
// disabled on this backend; imported UMA pools retain the original storage.
#include "nxrt/write_watch.h"
#include "nxrt/memory.h"
#include "nxrt/page_compare.h"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>

namespace nxrt::mem::ww {
struct Region {
    uint64_t base, pages;
    struct Page { std::array<uint8_t, 4096> bytes; bool valid = true; };
    std::map<uint64_t, Page> watched;
    std::set<uint64_t> written;
};
namespace {
std::mutex mu;
std::set<Region*> regions;
bool range(const Region* r, uint64_t first, uint64_t count) {
    return r && first <= r->pages && count <= r->pages - first;
}
bool same(const Region* r, uint64_t n, const Region::Page& p) {
    std::array<uint8_t, 4096> current;
    return p.valid && copy_readable(r->base + n * 4096, current.data(), current.size()) &&
           page_equal(current.data(), p.bytes.data());
}
}
Region* region_create(uint64_t addr, uint64_t size) {
    const auto reserved = heap_reserved();
    if (!size || addr < kHeapBase || addr - kHeapBase > reserved || size > reserved - (addr - kHeapBase)) return nullptr;
    const auto first = addr & ~uint64_t{4095}, end = (addr + size + 4095) & ~uint64_t{4095};
    auto* r = new Region{first, (end - first) / 4096, {}, {}};
    std::lock_guard lock(mu); regions.insert(r); return r;
}
void region_destroy(Region* r) { std::lock_guard lock(mu); if (regions.erase(r)) delete r; }
uint64_t region_base(const Region* r) { return r ? r->base : 0; }
uint64_t region_pages(const Region* r) { return r ? r->pages : 0; }
bool arm(Region* r, uint64_t first, uint64_t count) {
    std::lock_guard lock(mu);
    if (!range(r, first, count)) return false;
    std::map<uint64_t, Region::Page> pending;
    for (uint64_t n = first; n < first + count; ++n) {
        auto& p = pending[n];
        if (!copy_readable(r->base + n * 4096, p.bytes.data(), p.bytes.size())) return false;
    }
    // Transfer new page nodes instead of allocating a second full snapshot.
    // Existing pages stay in pending and replace their old bytes only after
    // the entire requested range has been read successfully.
    r->watched.merge(pending);
    for (auto& [n, p] : pending) r->watched.find(n)->second = std::move(p);
    r->written.erase(r->written.lower_bound(first), r->written.lower_bound(first + count));
    return true;
}
bool all_valid(const Region* r, uint64_t first, uint64_t count) {
    std::lock_guard lock(mu);
    if (!range(r, first, count)) return false;
    for (uint64_t n = first; n < first + count; ++n) {
        auto it = r->watched.find(n);
        if (it == r->watched.end() || !same(r, n, it->second)) return false;
    }
    return true;
}
bool page_valid(const Region* r, uint64_t n) { return all_valid(r, n, 1); }
bool page_banned(const Region* r, uint64_t n) { return !range(r, n, 1); }
void invalidate(Region* r, uint64_t first, uint64_t count) {
    std::lock_guard lock(mu);
    if (!range(r, first, count)) return;
    for (auto it = r->watched.lower_bound(first); it != r->watched.end() && it->first < first + count; ++it) it->second.valid = false;
}
void take_written_range(Region* r, uint64_t first, uint64_t count, std::vector<uint64_t>& out) {
    std::lock_guard lock(mu);
    if (!range(r, first, count)) return;
    const auto end = first + count;
    for (auto it = r->watched.lower_bound(first); it != r->watched.end() && it->first < end;) {
        if (!same(r, it->first, it->second)) { r->written.insert(it->first); it = r->watched.erase(it); }
        else ++it;
    }
    const auto begin = r->written.lower_bound(first), stop = r->written.lower_bound(end);
    out.insert(out.end(), begin, stop);
    r->written.erase(begin, stop);
}
void take_written(Region* r, std::vector<uint64_t>& out) {
    if (r) take_written_range(r, 0, r->pages, out);
}
void forget(uint64_t addr, uint64_t size, bool) {
    if (!size || size > UINT64_MAX - addr) return;
    std::lock_guard lock(mu);
    for (auto* r : regions) for (auto it = r->watched.begin(); it != r->watched.end();) {
        const auto page = r->base + it->first * 4096;
        if (page < addr + size && page + 4096 > addr) { r->written.insert(it->first); it = r->watched.erase(it); }
        else ++it;
    }
}
void before_os_write(const void* p, uint64_t n) { forget(reinterpret_cast<uint64_t>(p), n, false); }
void before_os_read(const void*, uint64_t) {} // No inaccessible read-guard pages exist.
OsWriteScope::OsWriteScope(const void* p, uint64_t n) { before_os_write(p, n); }
OsWriteScope::~OsWriteScope() = default;
OsReadScope::OsReadScope(const void*, uint64_t) {}
OsReadScope::~OsReadScope() = default;
void ban_section(uint64_t offset, uint64_t size) { forget(kHeapBase + offset, size, false); }
void set_guard_handler(GuardHandler) {} // guard() always rejects on this backend.
bool guard(Region*, uint64_t, uint64_t) { return false; }
void unguard(Region*, uint64_t, uint64_t) {}
bool all_armed(const Region*, uint64_t, uint64_t) { return false; }
uint64_t faults() { return 0; }
uint64_t fault_waits() { return 0; }
uint64_t guard_faults() { return 0; }
}
