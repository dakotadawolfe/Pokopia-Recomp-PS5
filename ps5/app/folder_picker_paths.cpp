// SPDX-License-Identifier: GPL-3.0-or-later
#include "folder_picker_paths.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nxrt::app {
namespace {
namespace fs = std::filesystem;
constexpr const char* supported_build = "76a489f3bd3642fa5e18bd83d32c98bacc5f7d0e";
bool path_text(const std::string& value) {
    if (value.empty() || value.size() > 4095 || value.front() != '/') return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool beneath(const fs::path& path, const fs::path& root) {
    auto a = path.begin(), b = root.begin();
    for (; b != root.end(); ++a, ++b) if (a == path.end() || *a != *b) return false;
    return true;
}
bool opened_metadata(const fs::path& path, struct stat& metadata, bool directory_only = false) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                     (directory_only ? O_DIRECTORY : 0));
    if (fd < 0) return false;
    const int result = fstat(fd, &metadata);
    const int closed = close(fd);
    return result == 0 && closed == 0;
}
bool directory(const fs::path& path) {
    struct stat metadata{};
    return opened_metadata(path, metadata, true) && S_ISDIR(metadata.st_mode);
}
fs::path checked_directory(const std::string& text) {
    if (!path_text(text)) return {};
    auto normalized = fs::path(text).lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    fs::path current;
    // The native loader's libc shim exports realpath as a null-returning stub.
    // Native apps also deny lstat. Inspect opened descriptors without following
    // symlinks, including every parent, to retain the storage-root boundary.
    for (const auto& component : normalized) {
        if (component.empty()) continue;
        current /= component;
        struct stat metadata{};
        if (!opened_metadata(current, metadata, true) || !S_ISDIR(metadata.st_mode)) return {};
    }
    return normalized;
}
uint32_t word(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
bool nso_header(const fs::path& path, std::string* build_id = nullptr) {
    struct stat metadata{};
    if (!opened_metadata(path, metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size < 0x100) return false;
    const uint64_t size = static_cast<uint64_t>(metadata.st_size);
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 0x100> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size()) || std::memcmp(header.data(), "NSO0", 4)) return false;
    for (unsigned n = 0; n < 3; ++n) {
        const uint64_t offset = word(header.data() + 0x10 + n * 0x10);
        const uint64_t bytes = word(header.data() + ((word(header.data() + 12) & (1u << n)) ? 0x60 + n * 4 : 0x18 + n * 0x10));
        if (offset < 0x100 || !bytes || offset > size || bytes > size - offset) return false;
    }
    if (build_id) {
        static constexpr char hex[] = "0123456789abcdef";
        build_id->clear();
        for (unsigned n = 0x40; n < 0x54; ++n) { build_id->push_back(hex[header[n] >> 4]); build_id->push_back(hex[header[n] & 15]); }
    }
    return true;
}
bool has_asset(const fs::path& root) {
    std::vector<fs::path> pending{root};
    size_t visited = 0;
    while (!pending.empty() && visited < 10000) {
        const auto path = pending.back(); pending.pop_back();
        std::error_code ec;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end && visited < 10000; it.increment(ec), ++visited) {
            struct stat metadata{};
            if (!opened_metadata(it->path(), metadata)) continue;
            if (S_ISDIR(metadata.st_mode)) pending.push_back(it->path());
            else if (S_ISREG(metadata.st_mode) && metadata.st_size > 0 && it->path().filename() != ".listing") {
                std::ifstream file(it->path(), std::ios::binary);
                char value = 0;
                if (file.get(value)) return true;
            }
        }
    }
    return false;
}
LocationCheck check_exefs(const fs::path& exefs, const std::vector<StorageRoot>& roots) {
    LocationCheck result;
    result.message = "This folder does not contain readable extracted executable files.";
    const auto canonical = picker_directory(exefs.string(), roots);
    if (canonical.empty()) return result;
    std::string build;
    for (const auto* name : {"main", "sdk", "subsdk0", "rtld"})
        if (!nso_header(fs::path(canonical) / name, std::strcmp(name, "main") == 0 ? &build : nullptr)) return result;
    if (build != supported_build) { result.message = "This executable build is not supported by this port."; return result; }
    const auto npdm_path = fs::path(canonical) / "main.npdm";
    std::error_code metadata_error;
    const auto metadata_bytes = fs::file_size(npdm_path, metadata_error);
    std::ifstream npdm(npdm_path, std::ios::binary);
    char magic[4]{};
    if (metadata_error || metadata_bytes < 0x80 || !npdm.read(magic, sizeof magic) || std::memcmp(magic, "META", 4)) {
        result.message = "The extracted executable folder needs a readable main.npdm file."; return result;
    }
    const fs::path selected_root = fs::path(canonical).parent_path();
    std::vector<fs::path> layers;
    if (selected_root.parent_path().filename() == "updates")
        layers.push_back(selected_root.parent_path().parent_path() / "base" / "romfs");
    layers.push_back(selected_root / "romfs");
    result.game = {selected_root.string(), canonical, build, {}};
    for (const auto& layer : layers) {
        const auto actual = picker_directory(layer.string(), roots);
        if (!actual.empty() && has_asset(actual) &&
            std::find(result.game.romfs_layers.begin(), result.game.romfs_layers.end(), actual) == result.game.romfs_layers.end())
            result.game.romfs_layers.push_back(actual);
    }
    if (result.game.romfs_layers.empty()) {
        result.message = "Executable files found, but no readable extracted romfs assets are beside them.";
        return result;
    }
    result.usable = true;
    result.message = result.game.romfs_layers.size() > 1 ? "Extracted game found with base and update asset folders." : "Extracted game files found. Use this folder to remember its location.";
    return result;
}
}

std::vector<StorageRoot> picker_storage_roots() {
    std::vector<StorageRoot> roots{{"INTERNAL STORAGE", "/data"}};
    for (unsigned i = 0; i < 8; ++i) roots.push_back({"USB " + std::to_string(i), "/mnt/usb" + std::to_string(i)});
    return roots;
}
std::string picker_directory(const std::string& path, const std::vector<StorageRoot>& roots) {
    const auto actual = checked_directory(path);
    if (actual.empty()) return {};
    for (const auto& root : roots) {
        const auto boundary = checked_directory(root.path);
        if (!boundary.empty() && beneath(actual, boundary)) return actual.string();
    }
    return {};
}
bool picker_package(const std::string& path) {
    auto extension = fs::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return extension == ".nsp" || extension == ".nsz" || extension == ".xci" || extension == ".xcz" || extension == ".nca";
}
LocationCheck check_game_location(const std::string& path, const std::vector<StorageRoot>& roots) {
    if (picker_package(path) && !directory(path)) return {false, {}, "Choose an extracted folder. NSP, NSZ, XCI and NCA files cannot be used here."};
    const auto actual = picker_directory(path, roots);
    if (actual.empty()) return {false, {}, "Folder is unavailable. Connect its drive or choose another folder."};
    fs::path selected(actual);
    if (selected.filename() == "exefs") return check_exefs(selected, roots);
    if (selected.filename() == "romfs") return check_exefs(selected.parent_path() / "exefs", roots);
    std::vector<fs::path> candidates;
    if (directory(selected / "exefs")) candidates.push_back(selected / "exefs");
    std::error_code ec;
    std::vector<fs::path> updates;
    for (fs::directory_iterator it(selected / "updates", ec), end; !ec && it != end; it.increment(ec)) {
        if (directory(it->path()) && directory(it->path() / "exefs")) updates.push_back(it->path());
    }
    std::sort(updates.begin(), updates.end(), [](const auto& a, const auto& b) {
        const auto left = a.filename().string(), right = b.filename().string();
        const auto numeric = [](const auto& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
        if (numeric(left) && numeric(right) && left.size() != right.size()) return left.size() > right.size();
        return left > right;
    });
    for (const auto& update : updates) candidates.push_back(update / "exefs");
    if (directory(selected / "base" / "exefs")) candidates.push_back(selected / "base" / "exefs");
    LocationCheck result{false, {}, "Open the extracted game folder containing exefs and romfs."};
    for (const auto& candidate : candidates) { result = check_exefs(candidate, roots); if (result.usable) return result; }
    return result;
}
bool load_picker_location(const std::string& config_file, std::string& path, std::string& error) {
    path.clear(); error.clear();
    std::error_code ec;
    if (!fs::exists(config_file, ec)) return !ec;
    const auto size = fs::file_size(config_file, ec);
    if (ec || !size || size > 4096) { error = "Saved folder setting is invalid."; return false; }
    std::ifstream file(config_file, std::ios::binary);
    std::string text(static_cast<size_t>(size), '\0');
    if (!file.read(text.data(), text.size())) { error = "Saved folder setting could not be read."; return false; }
    if (!text.empty() && text.back() == '\n') text.pop_back();
    if (!text.empty() && text.back() == '\r') text.pop_back();
    if (!path_text(text)) { error = "Saved folder setting is invalid."; return false; }
    path = std::move(text); return true;
}
bool save_picker_location(const std::string& config_file, const std::string& path, std::string& error) {
    error.clear();
    if (!path_text(path)) { error = "Folder path cannot be saved."; return false; }
    std::error_code ec; fs::create_directories(fs::path(config_file).parent_path(), ec);
    if (ec) { error = "Could not open the app settings folder."; return false; }
    static std::atomic<uint64_t> serial{0};
    const auto temporary = config_file + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(++serial);
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { error = "Could not create the folder setting."; return false; }
    const auto bytes = path + '\n'; size_t done = 0; bool okay = true;
    while (done < bytes.size()) {
        const auto count = write(fd, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<size_t>(count) > bytes.size() - done) { okay = false; break; }
        done += static_cast<size_t>(count);
    }
    if (okay && fsync(fd) != 0) okay = false;
    if (close(fd) != 0) okay = false;
    if (okay && rename(temporary.c_str(), config_file.c_str()) != 0) okay = false;
    if (!okay) { unlink(temporary.c_str()); error = "Could not save the folder setting. Your game files were not changed."; }
    return okay;
}
} // namespace nxrt::app
