// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_input.h"
#include "folder_picker_pad.h"
#include <chrono>
namespace nxrt::app {
namespace {
class NativeInput final : public hid::InputSource {
    PickerPad pad_;
public:
    void poll(hid::InputSnapshot& out) override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        out = {};
        out.npad[0] = game_pad(pad_.poll(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
    }
};
}
void install_game_input() { hid::set_input_source(std::make_shared<NativeInput>()); }
}
