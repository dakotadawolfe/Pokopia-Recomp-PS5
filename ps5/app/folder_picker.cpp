// SPDX-License-Identifier: GPL-3.0-or-later
#include "folder_picker.h"
#include "folder_picker_pad.h"
#include "folder_picker_paths.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace nxrt::app {
namespace fs = std::filesystem;
FolderPicker::FolderPicker(PickerOptions options) : options_(std::move(options)) {
    std::string remembered, error;
    const bool loaded = load_picker_location(options_.config_file, remembered, error);
    if (loaded && !remembered.empty() && !picker_directory(remembered, options_.roots).empty()) {
        browse(remembered);
        if (check_.usable) status_ = "Saved folder found. Use this folder or browse to change it.";
    } else {
        browse({});
        if (!remembered.empty()) status_ = "Saved folder unavailable. Connect its drive or choose another folder.";
        else if (!error.empty()) status_ = error;
    }
    refresh_view();
}
FolderPicker::~FolderPicker() = default;
void FolderPicker::browse(const std::string& path) {
    items_.clear(); selected_ = first_ = 0; check_ = {};
    directory_ = path.empty() ? std::string{} : picker_directory(path, options_.roots);
    if (directory_.empty()) {
        for (const auto& root : options_.roots) {
            const auto actual = picker_directory(root.path, options_.roots);
            std::printf("PICKER_STORAGE root=%s available=%d\n", root.path.c_str(), !actual.empty());
            if (!actual.empty()) items_.push_back({root.label, actual, Kind::Directory});
        }
        status_ = items_.empty() ? "No accessible storage. Connect a USB drive and press Square." : "Choose internal storage or a connected USB drive.";
        return;
    }
    check_ = check_game_location(directory_, options_.roots);
    status_ = check_.message;
    if (check_.usable) items_.push_back({"USE THIS FOLDER", directory_, Kind::Use});
    std::error_code ec;
    std::vector<Item> children;
    fs::directory_iterator it(directory_, ec), end;
    for (; !ec && it != end && children.size() < 1024; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        const auto status = it->status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            const auto actual = picker_directory(it->path().string(), options_.roots);
            if (!actual.empty()) children.push_back({name + "/", actual, Kind::Directory});
        } else if (fs::is_regular_file(status) && picker_package(name))
            children.push_back({name + " [PACKAGE]", it->path().string(), Kind::Package});
    }
    std::sort(children.begin(), children.end(), [](const Item& a, const Item& b) {
        if (a.kind != b.kind) return a.kind == Kind::Directory;
        return a.label < b.label;
    });
    items_.insert(items_.end(), children.begin(), children.end());
    if (ec) status_ = "Some folders could not be read. Check the drive or choose another folder.";
    else if (children.size() == 1024) status_ = "This folder is large. Showing its first 1024 entries.";
    else if (items_.empty()) status_ += " Press Circle to go back.";
}
void FolderPicker::refresh_view() {
    constexpr size_t visible = 12;
    if (items_.empty()) selected_ = first_ = 0;
    else {
        selected_ = std::min(selected_, items_.size() - 1);
        if (selected_ < first_) first_ = selected_;
        if (selected_ >= first_ + visible) first_ = selected_ - visible + 1;
    }
    view_.location = directory_.empty() ? "STORAGE" : directory_;
    view_.status = status_;
    view_.rows.clear();
    for (size_t n = first_; n < items_.size() && n < first_ + visible; ++n) view_.rows.push_back(items_[n].label);
    view_.selected = selected_ - first_;
}
void FolderPicker::activate() {
    if (items_.empty()) return;
    const auto item = items_[selected_];
    if (item.kind == Kind::Directory) { browse(item.path); return; }
    if (item.kind == Kind::Package) {
        status_ = "Choose an extracted folder. NSP, NSZ, XCI and NCA files cannot be used here."; return;
    }
    // The drive may have been removed while the confirmation row was visible.
    const auto checked = check_game_location(directory_, options_.roots);
    if (!checked.usable) { status_ = checked.message; return; }
    std::string error;
    if (!save_picker_location(options_.config_file, checked.game.root, error)) { status_ = error; return; }
    selection_ = checked.game; view_.done = true;
    status_ = "Folder saved. Game files stay on the selected drive.";
    pad_.reset();
}
void FolderPicker::back() {
    if (directory_.empty()) { view_.cancelled = true; pad_.reset(); return; }
    for (const auto& root : options_.roots) {
        if (picker_directory(root.path, options_.roots) == directory_) { browse({}); return; }
    }
    browse(fs::path(directory_).parent_path().string());
}
void FolderPicker::update() {
    if (view_.done || view_.cancelled) return;
    if (!pad_) pad_ = std::make_unique<PickerPad>();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto input = pad_->poll(static_cast<uint64_t>(now));
    const auto error = pad_->error();
    update(input, static_cast<uint64_t>(now));
    if (!input.connected && !error.empty()) view_.status = error;
}
void FolderPicker::update(const PickerInput& input, uint64_t now_ms) {
    if (view_.done || view_.cancelled) return;
    view_.controller_connected = input.connected;
    if (!input.connected) { armed_ = false; previous_ = {}; held_direction_ = 0; return; }
    if (!armed_) {
        armed_ = !input.select && !input.back && !input.refresh;
        previous_ = input;
        if (!armed_) return;
    }
    if (input.back && !previous_.back) back();
    else if (input.refresh && !previous_.refresh) browse(directory_);
    else if (input.select && !previous_.select) activate();
    else {
        const int direction = input.up == input.down ? 0 : input.up ? -1 : 1;
        if (direction && (direction != held_direction_ || now_ms >= next_repeat_)) {
            if (direction < 0 && selected_) --selected_;
            if (direction > 0 && selected_ + 1 < items_.size()) ++selected_;
            next_repeat_ = now_ms + (direction == held_direction_ ? 90 : 350);
        }
        held_direction_ = direction;
    }
    previous_ = input; refresh_view();
}
} // namespace nxrt::app
