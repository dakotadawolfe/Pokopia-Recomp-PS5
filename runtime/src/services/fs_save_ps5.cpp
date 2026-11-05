// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs_save_ps5.h"
#include "fs_volume.h"
#include "nxrt/config.h"
#include "nxrt/acc_ps5.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "nxrt/subpage.h"
#include "nxrt/log.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nxrt::ipc::detail { uint64_t note_unhandled(Service&, uint32_t); }
namespace nxrt::services::ps5save {
namespace {
namespace disk = std::filesystem;
using ipc::Request; using ipc::Response; using ipc::Service;
using Ptr = std::shared_ptr<Service>;
constexpr uint32_t full = (30u << 9) | 2;
struct Metadata { uint64_t magic = 0x315641535452584e; int64_t size = 0, journal = 0; };
struct Account {
    std::mutex mu;
    std::string path;
    Metadata metadata;
    std::shared_ptr<fs::Volume> volume;
    unsigned writers = 0;
};
std::mutex accounts_mu;
std::map<std::string, std::weak_ptr<Account>> accounts;
class StrictService final : public Service {
public:
    using Service::Service;
    uint32_t handle(Request& r, Response& s) override {
        if (cmds_.contains(r.cmd)) return Service::handle(r, s);
        if (ipc::detail::note_unhandled(*this, r.cmd) == 1)
            NXLOG_W("fs", "%s cmd %u is not ported", name().c_str(), r.cmd);
        return fs::rc::kNotImplemented;
    }
};
uint32_t args(const Request& r, size_t bytes, bool buffers = false) {
    if (!r.in_copy.empty() || !r.in_move.empty() || !r.in_services.empty()) return kernel::kInvalidHandle;
    if (r.in_size < bytes || (bytes && !r.in) || !r.w.empty()) return fs::rc::kInvalidArgument;
    if (!buffers && (!r.a.empty() || !r.b.empty() || !r.x.empty() || !r.c.empty())) return fs::rc::kInvalidArgument;
    return 0;
}
uint32_t path_arg(const Request& r, size_t index, std::string& path) {
    const auto b = r.in_buf(index);
    const uint64_t count = std::min<uint64_t>(b.size, 0x301);
    if (!count || !b.addr) return fs::rc::kInvalidArgument;
    if (!mem::SubpageMap::allows(b.addr, count, 1)) return kernel::kInvalidMemoryState;
    uint32_t error = 0;
    return fs::normalize_path(reinterpret_cast<const char*>(b.addr), count, &path, &error) ? 0 : error;
}
bool valid_user(uint64_t hi, uint64_t lo) {
    uint64_t expected_hi = 0, expected_lo = 0;
    return accapi::launch_user(&expected_hi, &expected_lo) && hi == expected_hi && lo == expected_lo;
}
std::string root_for(uint64_t hi, uint64_t lo) {
    char leaf[64];
    std::snprintf(leaf, sizeof leaf, "%016llx/%016llx%016llx",
        static_cast<unsigned long long>(program_id_now()), static_cast<unsigned long long>(hi),
        static_cast<unsigned long long>(lo));
    return config_get_str("fs", "save_dir", "/download0/pokopia-saves") + "/" + leaf;
}
bool sync_directory(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool okay = fsync(fd) == 0; close(fd); return okay;
}
uint32_t check_space(const std::string& path, uint64_t required) {
    const auto space = fs::available_storage(path);
    if (space.status == fs::SpaceStatus::Error) return fs::rc::kGenericFailure;
    if (space.status == fs::SpaceStatus::Measured) return space.bytes < required ? full : 0;
    NXLOG_I("fs", "Free-space query unavailable (errno=%d); enforcing save quota and actual I/O results", space.error);
    return 0;
}
bool store_metadata(const std::string& path, const Metadata& m) {
    const auto temp = path + "/metadata.pending", target = path + "/metadata";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool okay = write(fd, &m, sizeof m) == sizeof m && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!okay || !closed || rename(temp.c_str(), target.c_str())) { unlink(temp.c_str()); return false; }
    return sync_directory(path);
}
uint32_t load_metadata(const std::string& path, Metadata& m) {
    const int fd = open((path + "/metadata").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? fs::rc::kPathNotFound : fs::rc::kGenericFailure;
    struct stat st{};
    const bool okay = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof m && read(fd, &m, sizeof m) == sizeof m;
    close(fd);
    if (!okay || m.magic != Metadata{}.magic || m.size <= 0 || m.size > maximum_size ||
        m.journal < 0 || m.journal > maximum_size) return fs::rc::kGenericFailure;
    return 0;
}
uint32_t get_account(uint64_t hi, uint64_t lo, bool create, std::shared_ptr<Account>& out, bool* created = nullptr) {
    if (created) *created = false;
    if (!program_id_now() || !valid_user(hi, lo)) return fs::rc::kInvalidArgument;
    const auto path = root_for(hi, lo);
    std::lock_guard lock(accounts_mu);
    if (const auto it = accounts.find(path); it != accounts.end()) {
        if (auto live = it->second.lock()) { out = std::move(live); return 0; }
        accounts.erase(it);
    }
    auto a = std::make_shared<Account>(); a->path = path;
    uint32_t rc = load_metadata(path, a->metadata);
    if (rc == fs::rc::kPathNotFound && create) {
        const auto defaults = title_defaults();
        a->metadata.size = config_get_int("title", "account_save_size", defaults.account_save_size);
        a->metadata.journal = config_get_int("title", "account_save_journal", defaults.account_save_journal);
        if (a->metadata.size <= 0 || a->metadata.journal < 0) return fs::rc::kInvalidArgument;
        if (a->metadata.size > maximum_size || a->metadata.journal > maximum_size) return full;
        std::error_code ec;
        disk::create_directories(path, ec);
        if (ec) return fs::rc::kGenericFailure;
        if (const auto rc = check_space(path, a->metadata.size + std::max(a->metadata.size, a->metadata.journal))) return rc;
        // Publish metadata only after the real data directory and journal recover successfully.
        a->volume = std::make_shared<fs::Volume>(path + "/data", true, false, a->metadata.size);
        if (!a->volume->ok() || !store_metadata(path, a->metadata)) return fs::rc::kGenericFailure;
        if (created) *created = true;
        NXLOG_I("fs", "Created account save: size=%lld journal=%lld title_storage=true",
            static_cast<long long>(a->metadata.size), static_cast<long long>(a->metadata.journal));
    } else if (rc) return rc;
    if (!a->volume) {
        std::error_code ec;
        if (!disk::is_directory(path + "/data", ec) || ec) return fs::rc::kGenericFailure;
        a->volume = std::make_shared<fs::Volume>(path + "/data", true, false, a->metadata.size);
    }
    if (!a->volume->ok()) return fs::rc::kGenericFailure;
    accounts.emplace(path, a); out = std::move(a); return 0;
}
uint32_t growth(const Account& a, uint64_t old, uint64_t next) {
    if (next <= old) return 0;
    const auto used = a.volume->used_bytes(), cap = uint64_t(a.metadata.size);
    return used > cap || next - old > cap - used ? full : 0;
}
struct FileLease {
    std::shared_ptr<Account> account;
    std::shared_ptr<fs::VFile> file;
    bool writable;
    FileLease(std::shared_ptr<Account> a, std::shared_ptr<fs::VFile> f, bool w)
        : account(std::move(a)), file(std::move(f)), writable(w) { if (writable) ++account->writers; }
    ~FileLease() { std::lock_guard lock(account->mu); file.reset(); if (writable) --account->writers; }
};
Ptr file_service(const std::shared_ptr<Account>& a, std::shared_ptr<fs::VFile> f, bool writable) {
    auto lease = std::make_shared<FileLease>(a, std::move(f), writable);
    auto s = std::make_shared<StrictService>("nn::fssrv::sf::IFile");
    for (const bool write : {false, true}) s->add_cmd(write ? 1 : 0, write ? "Write" : "Read",
        [lease, write](Request& r, Response& out) -> uint32_t {
            if (const auto rc = args(r, 24, true)) return rc;
            const auto option = r.arg<uint32_t>(0); const auto offset = r.arg<int64_t>(8), size = r.arg<int64_t>(16);
            if (option > (write ? 1u : 0u)) return fs::rc::kInvalidArgument;
            if (offset < 0 || size < 0 || size > INT64_MAX - offset) return fs::rc::kOutOfRange;
            const auto b = write ? r.in_buf(0) : r.out_buf(0);
            if (uint64_t(size) > b.size) return fs::rc::kInvalidArgument;
            if (size && !mem::SubpageMap::allows(b.addr, size, write ? 1 : 2)) return kernel::kInvalidMemoryState;
            std::lock_guard lock(lease->account->mu);
            if (write) {
                if (!lease->writable) return fs::rc::kWriteNotPermitted;
                int64_t old = 0; if (const auto rc = lease->file->get_size(&old)) return rc;
                if (const auto rc = growth(*lease->account, old, offset + size)) return rc;
                if (const auto rc = lease->file->write(offset, reinterpret_cast<const void*>(b.addr), size)) return rc;
                return option ? lease->file->flush() : 0;
            }
            uint64_t got = 0;
            if (const auto rc = lease->file->read(offset, reinterpret_cast<void*>(b.addr), size, &got)) return rc;
            out.push<int64_t>(got); return 0;
        });
    s->add_cmd(2, "Flush", [lease](Request& r, Response&) -> uint32_t {
        if (const auto rc = args(r, 0)) return rc;
        std::lock_guard lock(lease->account->mu); return lease->file->flush();
    });
    s->add_cmd(3, "SetSize", [lease](Request& r, Response&) -> uint32_t {
        if (const auto rc = args(r, 8)) return rc;
        if (!lease->writable) return fs::rc::kWriteNotPermitted;
        const auto size = r.arg<int64_t>(0); if (size < 0) return fs::rc::kOutOfRange;
        std::lock_guard lock(lease->account->mu);
        int64_t old = 0; if (const auto rc = lease->file->get_size(&old)) return rc;
        if (const auto rc = growth(*lease->account, old, size)) return rc;
        return lease->file->set_size(size);
    });
    s->add_cmd(4, "GetSize", [lease](Request& r, Response& out) -> uint32_t {
        if (const auto rc = args(r, 0)) return rc;
        std::lock_guard lock(lease->account->mu); int64_t size = 0;
        if (const auto rc = lease->file->get_size(&size)) return rc;
        out.push(size); return 0;
    });
    return s;
}
Ptr directory_service(std::vector<fs::DirItem> entries) {
    struct Cursor { std::mutex mu; std::vector<fs::DirItem> entries; size_t position = 0; };
    auto cursor = std::make_shared<Cursor>(); cursor->entries = std::move(entries);
    auto s = std::make_shared<StrictService>("nn::fssrv::sf::IDirectory");
    s->add_cmd(0, "Read", [cursor](Request& r, Response& out) -> uint32_t {
        if (const auto rc = args(r, 0, true)) return rc;
        std::lock_guard lock(cursor->mu);
        const auto b = r.out_buf(0); const auto count = std::min<uint64_t>(b.size / 0x310, cursor->entries.size() - cursor->position);
        if (count && !mem::SubpageMap::allows(b.addr, count * 0x310, 2)) return kernel::kInvalidMemoryState;
        for (uint64_t i = 0; i < count; ++i) {
            const auto& e = cursor->entries[cursor->position++];
            auto* p = reinterpret_cast<uint8_t*>(b.addr) + i * 0x310; std::memset(p, 0, 0x310);
            std::memcpy(p, e.name.data(), std::min<size_t>(e.name.size(), 0x300)); p[0x304] = e.is_dir ? 0 : 1;
            std::memcpy(p + 0x308, &e.size, 8);
        }
        out.push<int64_t>(count); return 0;
    });
    s->add_cmd(1, "GetEntryCount", [cursor](Request& r, Response& out) -> uint32_t {
        if (const auto rc = args(r, 0)) return rc;
        out.push<int64_t>(cursor->entries.size()); return 0;
    });
    return s;
}
Ptr filesystem(const std::shared_ptr<Account>& a, bool ro) {
    auto s = std::make_shared<StrictService>("nn::fssrv::sf::IFileSystem");
    for (uint32_t cmd = 0; cmd <= 14; ++cmd) s->add_cmd(cmd, "SaveFileSystem",
        [a, ro, cmd](Request& r, Response& out) -> uint32_t {
            const size_t needed = cmd == 0 ? 16 : (cmd == 8 || cmd == 9 ? 4 : 0);
            if (const auto rc = args(r, needed, cmd != 10)) return rc;
            const bool mutating = cmd <= 6 || cmd == 13;
            if (ro && mutating) return fs::rc::kReadOnlyFileSystem;
            std::string p, to;
            if (cmd != 10) { if (const auto rc = path_arg(r, 0, p)) return rc; }
            if (cmd == 5 || cmd == 6) { if (const auto rc = path_arg(r, 1, to)) return rc; }
            std::lock_guard lock(a->mu);
            if (a->writers && (cmd == 1 || (cmd >= 3 && cmd <= 6) || cmd == 13)) return fs::rc::kTargetLocked;
            switch (cmd) {
            case 0: {
                if (r.arg<uint32_t>(0)) return fs::rc::kInvalidArgument;
                const auto size = r.arg<int64_t>(8); if (size < 0) return fs::rc::kOutOfRange;
                if (const auto rc = growth(*a, 0, size)) return rc;
                return a->volume->create_file(p, size);
            }
            case 1: return a->volume->delete_file(p);
            case 2: return a->volume->create_dir(p);
            case 3: return a->volume->delete_dir(p);
            case 4: return a->volume->delete_dir_recursive(p);
            case 5: return a->volume->rename_file(p, to);
            case 6: return a->volume->rename_dir(p, to);
            case 7: { uint32_t type = 0; if (const auto rc = a->volume->entry_type(p, &type)) return rc; out.push(type); return 0; }
            case 8: {
                const auto mode = r.arg<uint32_t>(0);
                if (!(mode & 3) || (mode & ~7u) || ((mode & fs::kModeAppend) && !(mode & fs::kModeWrite)))
                    return fs::rc::kInvalidArgument;
                if (ro && (mode & (fs::kModeWrite | fs::kModeAppend))) return fs::rc::kReadOnlyFileSystem;
                std::shared_ptr<fs::VFile> f;
                if (const auto rc = a->volume->open_file(p, mode, &f)) return rc;
                out.push_object(file_service(a, std::move(f), mode & fs::kModeWrite)); return 0;
            }
            case 9: {
                const auto mode = r.arg<uint32_t>(0);
                if (!(mode & 3) || (mode & ~0x80000003u)) return fs::rc::kInvalidArgument;
                std::vector<fs::DirItem> entries; if (const auto rc = a->volume->list_dir(p, &entries)) return rc;
                std::erase_if(entries, [mode](const auto& e) { return !(mode & (e.is_dir ? 1 : 2)); });
                if (mode & 0x80000000u) for (auto& e : entries) e.size = 0;
                out.push_object(directory_service(std::move(entries))); return 0;
            }
            case 10: return ro ? 0 : (a->writers ? fs::rc::kTargetLocked : a->volume->commit());
            case 11: { const auto used = a->volume->used_bytes(); out.push<int64_t>(used < uint64_t(a->metadata.size) ? a->metadata.size - used : 0); return 0; }
            case 12: out.push(a->metadata.size); return 0;
            case 13: return a->volume->clean_dir_recursive(p);
            case 14: {
                fs::FileTimes times; if (const auto rc = a->volume->file_times(p, &times)) return rc;
                out.push(times.created); out.push(times.modified); out.push(times.accessed); out.push<uint64_t>(times.valid); return 0;
            }
            default: return fs::rc::kNotImplemented;
            }
        });
    return s;
}
} // namespace

void reset() {
    std::lock_guard lock(accounts_mu);
    // Retained handles keep their volume. A new mount must not delete that
    // volume's work tree merely because the service registry was reset.
    std::erase_if(accounts, [](const auto& entry) { return entry.second.expired(); });
}
uint32_t extend(uint64_t hi, uint64_t lo, int64_t size, int64_t journal) {
    if (size <= 0 || journal < 0) return fs::rc::kInvalidArgument;
    if (size > maximum_size || journal > maximum_size) return full;
    std::shared_ptr<Account> a; if (const auto rc = get_account(hi, lo, false, a)) return rc;
    std::lock_guard lock(a->mu);
    if (size < a->metadata.size || journal < a->metadata.journal) return fs::rc::kInvalidArgument;
    if (const auto rc = check_space(a->path, uint64_t(size - a->metadata.size) * 2 + uint64_t(journal - a->metadata.journal))) return rc;
    Metadata next; next.size = size; next.journal = journal;
    if (!store_metadata(a->path, next)) return fs::rc::kGenericFailure;
    a->metadata = next; a->volume->set_capacity(size); return 0;
}
void add_commands(Service& s, std::shared_ptr<std::atomic<bool>> bound) {
    for (const bool ro : {false, true}) s.add_cmd(ro ? 53 : 51, ro ? "OpenReadOnlySaveDataFileSystem" : "OpenSaveDataFileSystem",
        [bound, ro](Request& r, Response& out) -> uint32_t {
            if (!bound->load(std::memory_order_acquire)) return kernel::kInvalidState;
            if (const auto rc = args(r, 0x48)) return rc;
            // User space, Account type, primary rank/index, current application only.
            const auto program = r.arg<uint64_t>(8);
            if (r.arg<uint8_t>(0) != 1 || (program && program != program_id_now()) ||
                r.arg<uint64_t>(32) || r.arg<uint8_t>(40) != 1 || r.arg<uint8_t>(41) || r.arg<uint16_t>(42))
                return fs::rc::kInvalidArgument;
            std::shared_ptr<Account> a;
            if (const auto rc = get_account(r.arg<uint64_t>(16), r.arg<uint64_t>(24), false, a)) return rc;
            out.push_object(filesystem(a, ro)); return 0;
        });
}
} // namespace nxrt::services::ps5save

namespace nxrt::services::fsapi {
uint32_t ensure_account_save(uint64_t hi, uint64_t lo, bool* created) {
    std::shared_ptr<ps5save::Account> a; return ps5save::get_account(hi, lo, true, a, created);
}
uint32_t account_save_size(uint64_t hi, uint64_t lo, int64_t* size, int64_t* journal) {
    if (!size || !journal) return fs::rc::kInvalidArgument;
    std::shared_ptr<ps5save::Account> a;
    if (const auto rc = ps5save::get_account(hi, lo, false, a)) return rc;
    std::lock_guard lock(a->mu); *size = a->metadata.size; *journal = a->metadata.journal; return 0;
}
}
