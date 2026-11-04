// SPDX-License-Identifier: GPL-3.0-or-later
#include "picker_storage_audit.h"
#include "folder_picker_paths.h"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace nxrt::app {
namespace {
bool write_fixture(const std::string& path, const void* bytes, size_t size) {
    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const auto written = write(fd, bytes, size);
    const int result = close(fd);
    return written == static_cast<ssize_t>(size) && result == 0;
}
bool check_native_selection() {
    const auto root = "/data/pokopia-recomp/tests/picker-audit-" + std::to_string(getpid());
    if (mkdir(root.c_str(), 0700)) return false;
    const auto game = root + "/game", config = root + "/location.txt";
    bool okay = mkdir(game.c_str(), 0700) == 0 && mkdir((game+"/exefs").c_str(), 0700) == 0 && mkdir((game+"/romfs").c_str(), 0700) == 0;
    try {
        std::array<unsigned char, 0x130> nso{};
        std::memcpy(nso.data(), "NSO0", 4);
        for (unsigned n = 0; n < 3; ++n) {
            const uint32_t offset = 0x100 + n * 0x10, bytes = 0x10;
            std::memcpy(nso.data()+0x10+n*0x10, &offset, 4);
            std::memcpy(nso.data()+0x18+n*0x10, &bytes, 4);
        }
        const unsigned char build[] = {0x76,0xa4,0x89,0xf3,0xbd,0x36,0x42,0xfa,0x5e,0x18,0xbd,0x83,0xd3,0x2c,0x98,0xba,0xcc,0x5f,0x7d,0x0e};
        std::memcpy(nso.data()+0x40, build, sizeof build);
        if (okay) for (const auto* name : {"main", "sdk", "subsdk0", "rtld"}) okay &= write_fixture(game+"/exefs/"+name, nso.data(), nso.size());
        std::array<char, 0x80> npdm{}; std::memcpy(npdm.data(), "META", 4);
        if (okay) okay = write_fixture(game+"/exefs/main.npdm", npdm.data(), npdm.size()) && write_fixture(game+"/romfs/synthetic.bin", "fixture", 7);
        const PickerOptions options{config, {{"DIAGNOSTIC", game}}};
        const auto checked = check_game_location(game, options.roots);
        std::printf("PICKER_AUDIT_FIXTURE layout=%d status=%s\n", checked.usable, checked.message.c_str());
        okay &= checked.usable;
        if (okay) {
            FolderPicker picker(options);
            picker.update({.connected=true}, 0); picker.update({.connected=true,.select=true}, 1);
            picker.update({.connected=true}, 2); picker.update({.connected=true,.select=true}, 3);
            std::string saved, error;
            const bool selected = picker.view().done && picker.path() == game;
            const bool persisted = load_picker_location(config, saved, error) && saved == game;
            FolderPicker remembered(options);
            const bool reopened = !remembered.view().rows.empty() && remembered.view().rows.front() == "USE THIS FOLDER";
            std::printf("PICKER_AUDIT_FIXTURE selected=%d persisted=%d reopened=%d status=%s\n", selected, persisted, reopened, picker.view().status.c_str());
            okay &= selected && persisted && reopened;
        }
    } catch (const std::exception& error) { std::printf("PICKER_AUDIT_FIXTURE exception=%s\n", error.what()); okay = false; }
    bool cleaned = true;
    for (const auto* name : {"main", "sdk", "subsdk0", "rtld", "main.npdm"})
        if (unlink((game+"/exefs/"+name).c_str()) && errno != ENOENT) cleaned = false;
    for (const auto& path : {game+"/romfs/synthetic.bin", config})
        if (unlink(path.c_str()) && errno != ENOENT) cleaned = false;
    for (const auto& path : {game+"/exefs", game+"/romfs", game, root})
        if (rmdir(path.c_str()) && errno != ENOENT) cleaned = false;
    std::printf("PICKER_AUDIT_FIXTURE cleanup=%d\n", cleaned);
    return okay && cleaned;
}
}
int run_picker_storage_audit() {
    constexpr const char* marker = "PICKER_STORAGE_AUDIT\n";
    const int trigger = open("/data/pokopia-recomp/tests/picker-storage-audit", O_RDONLY);
    if (trigger < 0) return -1;
    std::array<char, 64> text{};
    const auto count = read(trigger, text.data(), text.size());
    close(trigger);
    if (count != static_cast<ssize_t>(std::strlen(marker)) || std::memcmp(text.data(), marker, count)) return -1;
    std::printf("PICKER_AUDIT_BEGIN stat_size=%zu mode_offset=%zu size_offset=%zu\n",
        sizeof(struct stat), offsetof(struct stat, st_mode), offsetof(struct stat, st_size));
    auto roots = picker_storage_roots();
    roots.insert(roots.begin(), {"parent", "/"});
    roots.insert(roots.begin()+1, {"app-data", "/data/pokopia-recomp"});
    roots.insert(roots.begin()+2, {"mount-parent", "/mnt"});
    for (const auto& root : roots) {
        for (unsigned operation = 0; operation < 2; ++operation) {
            struct stat metadata{};
            errno = 0;
            const int result = operation ? stat(root.path.c_str(), &metadata) : lstat(root.path.c_str(), &metadata);
            const int error = errno;
            std::printf("PICKER_AUDIT path=%s call=%s result=%d errno=%d mode=%o bytes=%lld raw=",
                root.path.c_str(), operation ? "stat" : "lstat", result, error,
                unsigned(metadata.st_mode), static_cast<long long>(metadata.st_size));
            const auto* bytes = reinterpret_cast<const unsigned char*>(&metadata);
            for (size_t n = 0; n < 32; ++n) std::printf("%02x", bytes[n]);
            std::puts("");
        }
        errno = 0;
        DIR* handle = opendir(root.path.c_str());
        const int error = errno;
        size_t entries = 0;
        if (handle) { while (entries < 1024 && readdir(handle)) ++entries; closedir(handle); }
        std::printf("PICKER_AUDIT path=%s call=opendir opened=%d errno=%d entries=%zu\n",
            root.path.c_str(), handle != nullptr, error, entries);
        std::printf("PICKER_AUDIT path=%s picker=%s\n", root.path.c_str(), picker_directory(root.path, picker_storage_roots()).c_str());
    }
    const bool okay = !picker_directory("/data", picker_storage_roots()).empty() && check_native_selection();
    std::printf("PICKER_AUDIT_END native=true status=%s game_assets_modified=false\n", okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}
}
