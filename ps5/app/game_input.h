// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "folder_picker.h"
#include "nxrt/input_source.h"
#include <algorithm>
namespace nxrt::app {
inline hid::NpadInput game_pad(const PickerInput& sample) {
    hid::NpadInput out; out.connected = sample.connected;
    if (!out.connected) return out;
    using namespace hid;
    // Physical face-button positions match the guest layout: Circle=A, Cross=B.
    constexpr struct { uint32_t native; NpadButtonBit guest; } buttons[] = {
        {0x2000, kBtnA}, {0x4000, kBtnB}, {0x1000, kBtnX}, {0x8000, kBtnY},
        {2, kBtnStickL}, {4, kBtnStickR}, {0x400, kBtnL}, {0x800, kBtnR},
        {0x100, kBtnZL}, {0x200, kBtnZR}, {8, kBtnPlus}, {0x100000, kBtnMinus},
        {0x80, kBtnLeft}, {0x10, kBtnUp}, {0x20, kBtnRight}, {0x40, kBtnDown}
    };
    for (auto button : buttons) if (sample.native_buttons & button.native) out.buttons |= btn(button.guest);
    auto axis = [](uint8_t v) { return std::clamp((int(v) - 128) * 32767 / (v < 128 ? 128 : 127), -32767, 32767); };
    out.left = {axis(sample.lx), -axis(sample.ly)};
    out.right = {axis(sample.rx), -axis(sample.ry)};
    return out;
}
void install_game_input();
}
