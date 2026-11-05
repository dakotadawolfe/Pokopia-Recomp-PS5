// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX I/O for the shared save-volume implementation. Included inside its
// private namespace; the Windows implementation remains in fs_volume.cpp.
using HostHandle = int;
constexpr HostHandle invalid_host = -1;
constexpr int open_existing = 0, create_new = O_CREAT | O_EXCL;
uint64_t host_pid() { return static_cast<uint64_t>(getpid()); }
bool pid_alive(uint64_t pid) {
    return pid <= INT32_MAX && (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}
HostHandle open_host(const stdfs::path& p, bool write, int disposition) {
    return open(p.c_str(), (write ? O_RDWR : O_RDONLY) | disposition | O_CLOEXEC | O_NOFOLLOW, 0600);
}
bool sync_host(const stdfs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0; close(fd); return ok;
}
bool sync_parent(const stdfs::path& path) { return sync_host(path.parent_path()); }
bool sync_directory(const stdfs::path& path) { return sync_host(path); }
bool publish_marker(const stdfs::path& from, const stdfs::path& to) {
    return rename(from.c_str(), to.c_str()) == 0 && sync_parent(to);
}
bool safe_components(const stdfs::path& root, const stdfs::path& relative = {}) {
    stdfs::path current;
    const auto path = relative.empty() ? root : root / relative;
    for (const auto& component : path) {
        if (component.empty()) continue;
        current /= component;
        const int fd = open(current.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return errno == ENOENT;
        struct stat st{}; const bool valid = !fstat(fd, &st) && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode));
        close(fd); if (!valid) return false;
    }
    return true;
}
// Native applications deny lstat, including libc++ remove_all's use of it.
// Open without following a link and inspect the descriptor instead.
void remove_tree(const stdfs::path& path, std::error_code& ec) {
    ec.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { if (errno != ENOENT) ec.assign(errno, std::generic_category()); return; }
    struct stat st{};
    if (fstat(fd, &st)) { ec.assign(errno, std::generic_category()); close(fd); return; }
    if (!S_ISDIR(st.st_mode)) {
        close(fd); if (unlink(path.c_str())) ec.assign(errno, std::generic_category()); return;
    }
    DIR* dir = fdopendir(fd);
    if (!dir) { ec.assign(errno, std::generic_category()); close(fd); return; }
    for (;;) {
        errno = 0;
        const auto* entry = readdir(dir);
        if (!entry) { if (errno) ec.assign(errno, std::generic_category()); break; }
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        remove_tree(path / entry->d_name, ec);
        if (ec) break;
    }
    closedir(dir);
    if (!ec && rmdir(path.c_str())) ec.assign(errno, std::generic_category());
}
class HostFile final : public VFile {
public:
    HostFile(int fd, uint32_t mode) : fd_(fd), mode_(mode) {}
    ~HostFile() override { if (fd_ >= 0) close(fd_); }
    uint32_t read(int64_t offset, void* dst, uint64_t size, uint64_t* got) override {
        *got = 0;
        if (!(mode_ & kModeRead)) return rc::kReadNotPermitted;
        if (offset < 0 || size > uint64_t(INT64_MAX - offset)) return rc::kOutOfRange;
        while (size) {
            const auto chunk = static_cast<size_t>(std::min<uint64_t>(size, 1u << 26));
            const auto n = pread(fd_, static_cast<uint8_t*>(dst) + *got, chunk, offset);
            if (n < 0) { if (errno == EINTR) continue; return rc::kGenericFailure; }
            if (!n) break;
            *got += n; offset += n; size -= n;
        }
        return 0;
    }
    uint32_t write(int64_t offset, const void* src, uint64_t size) override {
        if (!(mode_ & kModeWrite)) return rc::kWriteNotPermitted;
        if (offset < 0 || size > uint64_t(INT64_MAX - offset)) return rc::kOutOfRange;
        std::lock_guard lock(mutex_);
        int64_t length = 0;
        if (const auto r = get_size(&length)) return r;
        if (uint64_t(offset) + size > uint64_t(length) && !(mode_ & kModeAppend)) return rc::kNoAppend;
        uint64_t done = 0;
        while (size) {
            const auto chunk = static_cast<size_t>(std::min<uint64_t>(size, 1u << 26));
            const auto n = pwrite(fd_, static_cast<const uint8_t*>(src) + done, chunk, offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return rc::kGenericFailure;
            done += n; offset += n; size -= n;
        }
        return 0;
    }
    uint32_t flush() override { return !(mode_ & kModeWrite) || !fsync(fd_) ? 0 : rc::kGenericFailure; }
    uint32_t set_size(int64_t size) override {
        if (!(mode_ & kModeWrite)) return rc::kWriteNotPermitted;
        if (size < 0) return rc::kOutOfRange;
        std::lock_guard lock(mutex_);
        return ftruncate(fd_, size) == 0 ? 0 : rc::kGenericFailure;
    }
    uint32_t get_size(int64_t* size) override {
        struct stat st{};
        if (fstat(fd_, &st)) return rc::kGenericFailure;
        *size = st.st_size; return 0;
    }
private:
    int fd_;
    uint32_t mode_;
    std::mutex mutex_;
};
class RootLock {
public:
    explicit RootLock(const std::string& root) {
        fd_ = open((root + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ >= 0) {
            int r;
            do { r = flock(fd_, LOCK_EX); } while (r < 0 && errno == EINTR);
            if (r) { close(fd_); fd_ = -1; }
        }
    }
    bool ok() const { return fd_ >= 0; }
    ~RootLock() { if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); } }
private:
    int fd_ = -1;
};
