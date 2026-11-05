// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace nxrt::ps5_graphics {
// Offline GameShare callers still request window identities during startup.
// Borrow the native window already owned by the primary display. No remote
// display, GPU-driver session, or streaming connection is created here.
class OfflineGuestDisplays {
public:
    bool create(int32_t count, uint64_t native_window) {
        if (count_ || count < 1 || count > 3 || !native_window) return false;
        count_ = static_cast<uint32_t>(count);
        window_ = native_window;
        return true;
    }
    uint64_t window(uint32_t guest) const { return guest < count_ ? window_ : 0; }
    bool active() const { return count_ != 0; }
    void destroy() { count_ = 0; window_ = 0; }
private:
    uint32_t count_ = 0;
    uint64_t window_ = 0;
};
}
