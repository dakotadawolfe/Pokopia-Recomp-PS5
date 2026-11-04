// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/host_vm.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#ifdef NXRT_TARGET_PS5
#include "nxrt/ps5_vm.h"
#include <cstring>
#endif
#ifndef __FreeBSD__
#include <sys/syscall.h>
#endif

namespace nxrt::host_vm {
#ifdef NXRT_TARGET_PS5
struct NativeMapping { ps5_vm::Reservation view; explicit NativeMapping(ps5_vm::Reservation value) : view(std::move(value)) {} };
struct NativeSharedObject { ps5_vm::DirectMemory backing; };
#else
struct NativeMapping {};
struct NativeSharedObject {};
#endif
namespace {
bool aligned(size_t value) {
    const size_t page = page_size();
    return page && value % page == 0;
}
bool valid_mapping(size_t size, uintptr_t address) {
    if (!size || !aligned(size) || !aligned(address)) {
        errno = EINVAL;
        return false;
    }
    if (size > std::numeric_limits<uintptr_t>::max() - address) {
        errno = EOVERFLOW;
        return false;
    }
    return true;
}
int permissions(Access access) {
    switch (access) {
        case Access::None: return PROT_NONE;
        case Access::Read: return PROT_READ;
        case Access::ReadWrite: return PROT_READ | PROT_WRITE;
    }
    return -1;
}
void* map_exact(size_t size, uintptr_t address, int protection, int flags, int fd, off_t offset) {
    void* mapped = mmap(reinterpret_cast<void*>(address), size, protection, flags, fd, offset);
    if (mapped == MAP_FAILED) return MAP_FAILED;
    if (address && reinterpret_cast<uintptr_t>(mapped) != address) {
        // mmap without MAP_FIXED leaves occupied addresses alone. Release only
        // the alternative mapping it gave us, never the requested range.
        if (munmap(mapped, size) != 0) std::perror("host_vm: release alternative mapping");
        errno = EEXIST;
        return MAP_FAILED;
    }
    return mapped;
}
}

size_t page_size() {
    static const size_t value = [] {
        const long n = sysconf(_SC_PAGESIZE);
        return n > 0 ? static_cast<size_t>(n) : size_t{0};
    }();
    return value;
}

Mapping::Mapping() = default;
SharedObject::SharedObject() = default;
Mapping::Mapping(void* address, size_t size) : address_(address), size_(size) {}
SharedObject::SharedObject(int fd, size_t size) : fd_(fd), size_(size) {}
Mapping::~Mapping() {
    if (!reset()) std::perror("host_vm: release mapping");
}
Mapping::Mapping(Mapping&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), size_(std::exchange(other.size_, 0)), native_(std::move(other.native_)) {}
Mapping Mapping::reserve(size_t size, uintptr_t address) {
    if (!valid_mapping(size, address)) return {};
    void* p = map_exact(size, address, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? Mapping{} : Mapping{p, size};
}
bool Mapping::protect(size_t offset, size_t size, Access access) {
    const int prot = permissions(access);
    if (!size_ || !size || !aligned(offset) || !aligned(size) ||
        offset > size_ || size > size_ - offset || prot < 0) {
        errno = EINVAL;
        return false;
    }
#ifdef NXRT_TARGET_PS5
    if (native_) return native_->view.protect(offset, size, access);
#endif
    return mprotect(static_cast<unsigned char*>(address_) + offset, size, prot) == 0;
}
bool Mapping::reset() {
#ifdef NXRT_TARGET_PS5
    if (native_) {
        if (!native_->view.reset()) return false;
        native_.reset(); address_ = nullptr; size_ = 0; return true;
    }
#endif
    if (size_ && munmap(address_, size_) != 0) return false;
    address_ = nullptr;
    size_ = 0;
    return true;
}

SharedObject::~SharedObject() {
    // Never retry close: an interrupted close may already have released the fd.
    if (fd_ >= 0 && close(fd_) != 0) std::perror("host_vm: close shared object");
}
SharedObject::SharedObject(SharedObject&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), size_(std::exchange(other.size_, 0)), native_(std::move(other.native_)) {}
SharedObject SharedObject::create(size_t size) {
    if (!valid_mapping(size, 0)) return {};
#ifdef NXRT_TARGET_PS5
    auto native = std::make_unique<NativeSharedObject>();
    native->backing = ps5_vm::DirectMemory::allocate(size);
    if (!native->backing) return {};
    auto scratch = ps5_vm::Reservation::reserve(size);
    if (!scratch || !scratch.map(0, native->backing, 0, size, Access::ReadWrite)) return {};
    std::memset(scratch.data(), 0, size);
    SharedObject result; result.size_ = size; result.native_ = std::move(native);
    return result;
#else
    if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        errno = EOVERFLOW;
        return {};
    }
#ifdef __FreeBSD__
    int fd = shm_open(SHM_ANON, O_RDWR, 0600);
#else
    int fd = static_cast<int>(syscall(SYS_memfd_create, "nxrt-heap", 0));
#endif
    if (fd < 0) return {};
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return {};
    }
    return SharedObject{fd, size};
#endif
}
Mapping SharedObject::map(size_t offset, size_t size, Access access, uintptr_t address) const {
    if (!valid_mapping(size, address)) return {};
    const int prot = permissions(access);
    if (!*this || !aligned(offset) || offset > size_ || size > size_ - offset || prot < 0) {
        errno = EINVAL;
        return {};
    }
#ifdef NXRT_TARGET_PS5
    if (native_) {
        auto view = ps5_vm::Reservation::reserve(size, address);
        if (!view || !view.map(0, native_->backing, offset, size, access)) return {};
        Mapping result;
        result.native_ = std::make_unique<NativeMapping>(std::move(view));
        result.address_ = result.native_->view.data(); result.size_ = size;
        return result;
    }
#endif
    void* p = map_exact(size, address, prot, MAP_SHARED, fd_, static_cast<off_t>(offset));
    return p == MAP_FAILED ? Mapping{} : Mapping{p, size};
}

} // namespace nxrt::host_vm
