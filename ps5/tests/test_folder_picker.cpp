// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "folder_picker.h"
#include "folder_picker_paths.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
NXTEST_GROUP(folder_picker)
#ifdef NXRT_PICKER_NO_REALPATH
// Model the native libc shim so host checks cannot hide an accidental return
// to canonical()/realpath() in production folder navigation.
extern "C" char* __wrap_realpath(const char*, char*) { errno = ENOSYS; return nullptr; }
static bool deny_lstat = false;
extern "C" int __real_lstat(const char*, struct stat*);
extern "C" int __wrap_lstat(const char* path, struct stat* metadata) {
    if (deny_lstat) { errno = EPERM; return -1; }
    return __real_lstat(path, metadata);
}
#endif
namespace {
namespace fs = std::filesystem;
using namespace nxrt::app;
struct Fixture {
    fs::path path;
    Fixture() {
        const auto base = std::getenv("NXRT_TEST_TEMP");
        auto pattern = ((base ? fs::path(base) : fs::temp_directory_path()) / "nxrt-picker-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("cannot create picker fixture");
        path = fs::path(pattern).lexically_normal();
#ifdef NXRT_PICKER_NO_REALPATH
        deny_lstat = true;
#endif
    }
    ~Fixture() {
#ifdef NXRT_PICKER_NO_REALPATH
        deny_lstat = false; // Host fixture cleanup is outside the app's API surface.
#endif
        if (path.filename().string().starts_with("nxrt-picker-")) { std::error_code ec; fs::remove_all(path, ec); }
    }
    std::vector<StorageRoot> roots() const { return {{"TEST STORAGE", path.string()}}; }
    PickerOptions options() const { return {(path / "settings/location.txt").string(), roots()}; }
};
void bytes(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file.write(value.data(), value.size());
    if (!file) throw std::runtime_error("fixture write failed");
}
void game(const fs::path& root) {
    for (const char* name : {"main", "sdk", "subsdk0", "rtld"}) {
        std::string nso(0x130, '\0'); std::memcpy(nso.data(), "NSO0", 4);
        for (unsigned n = 0; n < 3; ++n) {
            const uint32_t offset = 0x100 + n * 0x10, address = n * 0x1000, size = 0x10;
            std::memcpy(nso.data() + 0x10 + n * 0x10, &offset, 4);
            std::memcpy(nso.data() + 0x14 + n * 0x10, &address, 4);
            std::memcpy(nso.data() + 0x18 + n * 0x10, &size, 4);
        }
        const std::array<uint8_t, 20> build{0x76,0xa4,0x89,0xf3,0xbd,0x36,0x42,0xfa,0x5e,0x18,0xbd,0x83,0xd3,0x2c,0x98,0xba,0xcc,0x5f,0x7d,0x0e};
        std::memcpy(nso.data() + 0x40, build.data(), build.size()); bytes(root / "exefs" / name, nso);
    }
    std::string npdm(0x80, '\0'); std::memcpy(npdm.data(), "META", 4);
    bytes(root / "exefs/main.npdm", npdm); bytes(root / "romfs/test.bin", "synthetic fixture");
}
void neutral(FolderPicker& picker, uint64_t time = 0) { picker.update({.connected=true}, time); }
void press(FolderPicker& picker, PickerInput input, uint64_t time = 100) {
    neutral(picker, time); input.connected = true; picker.update(input, time + 1);
}
}
NXTEST(picker_checks_flat_and_direct_exefs_romfs_without_changing_assets) {
    Fixture f; const auto root = f.path / "Pokopia"; game(root);
    for (const auto& path : {root, root / "exefs", root / "romfs"}) {
        const auto result = check_game_location(path.string(), f.roots()); REQUIRE(result.usable);
        CHECK_EQ(result.game.root, root.string()); CHECK_EQ(result.game.exefs, (root / "exefs").string());
        REQUIRE(result.game.romfs_layers.size() == 1); CHECK_EQ(result.game.romfs_layers[0], (root / "romfs").string());
    }
    CHECK_EQ(fs::file_size(root / "romfs/test.bin"), 17u);
}
NXTEST(picker_directory_checks_kernel_metadata_and_rejects_symlink_escape) {
    Fixture f;
    fs::create_directories(f.path / "inside/nested");
    fs::create_directory_symlink(f.path / "inside", f.path / "inside-link");
    fs::create_directory_symlink(f.path.parent_path(), f.path / "outside-link");
    CHECK_EQ(picker_directory((f.path / "inside/./nested/..").string(), f.roots()), (f.path / "inside").string());
    CHECK(picker_directory((f.path / "inside-link").string(), f.roots()).empty());
    CHECK(picker_directory((f.path / "outside-link").string(), f.roots()).empty());
    CHECK(picker_directory(f.path.parent_path().string(), f.roots()).empty());
    CHECK(picker_directory("relative/folder", f.roots()).empty());
    CHECK(picker_directory((f.path / "missing").string(), f.roots()).empty());
}
NXTEST(picker_selects_latest_matching_update_and_preserves_base_layer_order) {
    Fixture f; const auto root = f.path / "Pokopia";
    game(root / "base"); game(root / "updates/9"); game(root / "updates/10");
    const auto result = check_game_location(root.string(), f.roots()); REQUIRE(result.usable);
    CHECK_EQ(result.game.root, (root / "updates/10").string()); REQUIRE(result.game.romfs_layers.size() == 2);
    CHECK_EQ(result.game.romfs_layers[0], (root / "base/romfs").string());
    CHECK_EQ(result.game.romfs_layers[1], (root / "updates/10/romfs").string());
    CHECK_EQ(check_game_location((root / "updates/9").string(), f.roots()).game.root, (root / "updates/9").string());
}
NXTEST(picker_rejects_containers_wrong_build_truncated_images_and_missing_assets) {
    Fixture f; const auto root = f.path / "Pokopia"; game(root);
    bytes(f.path / "Pokopia.NSP", "package");
    CHECK(check_game_location((f.path / "Pokopia.NSP").string(), f.roots()).message.find("extracted") != std::string::npos);
    bytes(root / "exefs/main", "NSO0"); CHECK(!check_game_location(root.string(), f.roots()).usable);
    game(root);
    { std::fstream main(root / "exefs/main", std::ios::in | std::ios::out | std::ios::binary); main.seekp(0x40); main.put(0); }
    CHECK(check_game_location(root.string(), f.roots()).message.find("not supported") != std::string::npos);
    game(root); fs::remove(root / "romfs/test.bin");
    CHECK(check_game_location(root.string(), f.roots()).message.find("no readable") != std::string::npos);
    CHECK(!check_game_location("/", f.roots()).usable);
}
NXTEST(picker_config_is_atomic_bounded_and_does_not_replace_valid_setting_on_invalid_input) {
    Fixture f; const auto config = f.options().config_file; std::string saved, error;
    REQUIRE(load_picker_location(config, saved, error)); CHECK(saved.empty());
    REQUIRE(save_picker_location(config, (f.path / "first").string(), error));
    CHECK(!save_picker_location(config, "/data/bad\nsecond", error));
    REQUIRE(load_picker_location(config, saved, error)); CHECK_EQ(saved, (f.path / "first").string());
    bytes(config, std::string(4097, 'x')); CHECK(!load_picker_location(config, saved, error));
    CHECK(!error.empty());
}
NXTEST(picker_remembers_folder_requires_fresh_confirm_and_rechecks_disconnected_storage) {
    Fixture f; const auto root = f.path / "Pokopia"; game(root); std::string error;
    REQUIRE(save_picker_location(f.options().config_file, root.string(), error));
    FolderPicker picker(f.options()); REQUIRE(!picker.view().rows.empty()); CHECK_EQ(picker.view().rows[0], "USE THIS FOLDER");
    picker.update({.connected=true,.select=true}, 0); CHECK(!picker.view().done);
    press(picker, {.select=true}); REQUIRE(picker.view().done); CHECK_EQ(picker.selection().root, root.string());
    FolderPicker unplugged(f.options()); neutral(unplugged);
    fs::rename(root, f.path / "gone"); press(unplugged, {.select=true}); CHECK(!unplugged.view().done);
    CHECK(unplugged.view().status.find("unavailable") != std::string::npos);
    FolderPicker missing(f.options()); CHECK_EQ(missing.view().location, "STORAGE");
    CHECK(missing.view().status.find("Saved folder unavailable") != std::string::npos); CHECK(!missing.view().cancelled);
}
NXTEST(picker_package_status_and_cancel_are_explicit) {
    Fixture f; bytes(f.path / "Pokopia.nsp", "package"); FolderPicker picker(f.options());
    press(picker, {.select=true}); REQUIRE(picker.view().rows.size() == 1);
    press(picker, {.select=true}, 200); CHECK(picker.view().status.find("extracted") != std::string::npos);
    CHECK(!picker.view().done && !picker.view().cancelled);
    press(picker, {.back=true}, 300); CHECK_EQ(picker.view().location, "STORAGE");
    press(picker, {.back=true}, 400); CHECK(picker.view().cancelled);
    CHECK(!fs::exists(f.options().config_file));
}
NXTEST(picker_navigation_repeats_scrolls_and_does_not_repeat_confirmation) {
    Fixture f; for (unsigned n = 0; n < 16; ++n) fs::create_directory(f.path / ("folder" + std::to_string(n)));
    FolderPicker picker(f.options()); press(picker, {.select=true}); REQUIRE(picker.view().rows.size() == 12);
    picker.update({.connected=true,.select=true}, 150); CHECK_EQ(picker.view().location, f.path.string());
    press(picker, {.down=true}, 200); CHECK_EQ(picker.view().selected, 1u);
    picker.update({.connected=true,.down=true}, 500); CHECK_EQ(picker.view().selected, 1u);
    picker.update({.connected=true,.down=true}, 551); CHECK_EQ(picker.view().selected, 2u);
    for (uint64_t time = 650; time < 2200; time += 100) picker.update({.connected=true,.down=true}, time);
    CHECK_EQ(picker.view().selected, 11u); CHECK_EQ(picker.view().rows.back(), "folder9/");
    CHECK(!picker.view().done);
}
