// SPDX-License-Identifier: GPL-3.0-or-later
#include "folder_picker_pad.h"
#include <array>
#include <cstddef>
#include <cstdio>

#ifdef NXRT_TARGET_PS5
namespace {
// Public ScePad data ABI, also used by the local PS5 PadBridge implementation.
struct PadTouch { uint16_t x, y; uint8_t id, reserved[3]; };
struct PadTouches { uint8_t count, reserved[3]; uint32_t reserved2; PadTouch touches[2]; };
struct PadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    PadTouches touches;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t extension[16], connected_count, reserved[2], unique_length, unique[12];
};
static_assert(sizeof(PadData) == 120 && offsetof(PadData, connected) == 76 && offsetof(PadData, timestamp) == 80);
extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceGetInitialUser(int*);
int sceUserServiceGetForegroundUser(int*);
int sceUserServiceGetLoginUserIdList(int*);
int scePadInit();
int scePadGetHandle(int, int, int);
int scePadOpen(int, int, int, const void*);
int scePadClose(int);
int scePadReadState(int, PadData*);
}
}
#endif
namespace nxrt::app {
PickerPad::~PickerPad() { close(); }
void PickerPad::close() {
#ifdef NXRT_TARGET_PS5
    if (handle_ >= 0 && owned_) scePadClose(handle_);
#endif
    handle_ = -1; owned_ = false;
}
PickerInput PickerPad::poll(uint64_t now_ms) {
    PickerInput input{};
#ifdef NXRT_TARGET_PS5
    if (handle_ < 0) {
        if (now_ms < retry_at_) return input;
        retry_at_ = now_ms + 1000;
        if (!initialized_) {
            // Another native component may already own library initialization.
            // A successful handle/read below is the authoritative readiness check.
            const int user_result = sceUserServiceInitialize(nullptr), pad_result = scePadInit();
            std::printf("PICKER_PAD init user_service=%08x pad=%08x\n",
                static_cast<unsigned>(user_result), static_cast<unsigned>(pad_result));
            initialized_ = true;
        }
        std::array<int, 6> users{-1, -1, -1, -1, -1, -1};
        sceUserServiceGetInitialUser(&users[0]); sceUserServiceGetForegroundUser(&users[1]);
        std::array<int, 4> logged{-1, -1, -1, -1};
        if (sceUserServiceGetLoginUserIdList(logged.data()) == 0)
            for (size_t n = 0; n < logged.size(); ++n) users[n + 2] = logged[n];
        for (const int user : users) {
            if (user <= 0) continue;
            handle_ = scePadGetHandle(user, 0, 0);
            if (handle_ >= 0) {
                owned_ = false;
                std::printf("PICKER_PAD handle=%x user=%x ownership=borrowed\n", handle_, user);
                break;
            }
            handle_ = scePadOpen(user, 0, 0, nullptr);
            if (handle_ >= 0) {
                owned_ = true;
                std::printf("PICKER_PAD handle=%x user=%x ownership=opened\n", handle_, user);
                break;
            }
        }
        if (handle_ < 0) { error_ = "Connect a controller and sign in. Waiting for controller input..."; return input; }
    }
    PadData data{};
    const int result = scePadReadState(handle_, &data);
    if (result != 0) {
        std::printf("PICKER_PAD read_failed=%08x\n", static_cast<unsigned>(result));
        close(); retry_at_ = now_ms + 1000;
        error_ = "Controller unavailable. Reconnect it to continue."; return input;
    }
    if (!data.connected) { error_ = "Connect a controller to choose the game folder."; return input; }
    if (data.buttons & 0x80000000u) { error_ = "Return from the PS5 menu to continue choosing a folder."; return input; }
    error_.clear(); input.connected = true;
    input.native_buttons = data.buttons;
    input.lx = data.lx; input.ly = data.ly; input.rx = data.rx; input.ry = data.ry;
    input.up = (data.buttons & 0x10) || data.ly < 64;
    input.down = (data.buttons & 0x40) || data.ly > 192;
    input.select = data.buttons & 0x4000;
    input.back = data.buttons & 0x2000;
    input.refresh = data.buttons & 0x8000;
#else
    (void)now_ms;
    error_ = "Native controller polling is available on PS5.";
#endif
    return input;
}
} // namespace nxrt::app
