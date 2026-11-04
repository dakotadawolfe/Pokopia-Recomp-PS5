// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
namespace nxrt::ps5_graphics {
// Owns the native renderer for the bounded game-startup diagnostic.
bool open_picker();
bool startup_status(std::string_view message); // loader thread, before guest dispatch
bool install_game_bridge(); // mapped/registered/protected game modules required
void report();
void shutdown(); // only after all guest workers have stopped
}
