// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::app {
struct StorageRoot { std::string label, path; };
struct GameLocation {
    std::string root, exefs, build_id;
    std::vector<std::string> romfs_layers;
};
struct LocationCheck { bool usable = false; GameLocation game; std::string message; };
std::vector<StorageRoot> picker_storage_roots();
LocationCheck check_game_location(const std::string&, const std::vector<StorageRoot>&);
bool load_picker_location(const std::string& config_file, std::string& path, std::string& error);
bool save_picker_location(const std::string& config_file, const std::string& path, std::string& error);

struct PickerInput {
    bool connected = false, up = false, down = false;
    bool select = false, back = false, refresh = false;
    uint32_t native_buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};
struct PickerView {
    std::string title = "CHOOSE POKOPIA FOLDER", location, status;
    std::string controls = "D-PAD / LEFT STICK: MOVE   X: OPEN / USE   O: BACK   SQUARE: REFRESH";
    std::vector<std::string> rows;
    size_t selected = 0;
    bool done = false, cancelled = false, controller_connected = false;
};
struct PickerOptions {
    std::string config_file = "/data/pokopia-recomp/game-location.txt";
    std::vector<StorageRoot> roots = picker_storage_roots();
};
class PickerPad;
class FolderPicker {
public:
    explicit FolderPicker(PickerOptions = {});
    ~FolderPicker();
    FolderPicker(const FolderPicker&) = delete;
    FolderPicker& operator=(const FolderPicker&) = delete;
    void update();
    void update(const PickerInput&, uint64_t now_ms);
    const PickerView& view() const { return view_; }
    const GameLocation& selection() const { return selection_; }
    const std::string& path() const { return selection_.root; }
private:
    enum class Kind { Directory, Package, Use };
    struct Item { std::string label, path; Kind kind; };
    PickerOptions options_;
    PickerView view_;
    GameLocation selection_;
    LocationCheck check_;
    std::string directory_, status_;
    std::vector<Item> items_;
    size_t selected_ = 0, first_ = 0;
    PickerInput previous_{};
    bool armed_ = false;
    int held_direction_ = 0;
    uint64_t next_repeat_ = 0;
    std::unique_ptr<PickerPad> pad_;
    void browse(const std::string&);
    void refresh_view();
    void activate();
    void back();
};
} // namespace nxrt::app
