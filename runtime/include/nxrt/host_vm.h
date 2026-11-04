// SPDX-License-Identifier: GPL-3.0-or-later
// Native VM ownership for the POSIX runtime. Callers serialize changes to each
// object. Sizes and offsets must use host pages: guest 4 KiB permissions belong
// in the guest memory layer and must never be silently rounded here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nxrt::host_vm {

enum class Access { None, Read, ReadWrite };
size_t page_size();

class SharedObject;
struct NativeMapping;
struct NativeSharedObject;
class Mapping {
public:
    Mapping();
    ~Mapping();
    Mapping(Mapping&& other) noexcept;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping& operator=(Mapping&&) = delete;

    // Anonymous, initially inaccessible address space. A nonzero address is an
    // exact requirement; an occupied range is never overwritten.
    static Mapping reserve(size_t size, uintptr_t address = 0);
    explicit operator bool() const { return size_ != 0; }
    void* data() const { return address_; }
    size_t size() const { return size_; }
    bool protect(size_t offset, size_t size, Access access);
    bool reset();

private:
    friend class SharedObject;
    Mapping(void* address, size_t size);
    void* address_ = nullptr;
    size_t size_ = 0;
    std::unique_ptr<NativeMapping> native_;
};

class SharedObject {
public:
    SharedObject();
    ~SharedObject();
    SharedObject(SharedObject&& other) noexcept;
    SharedObject(const SharedObject&) = delete;
    SharedObject& operator=(const SharedObject&) = delete;
    SharedObject& operator=(SharedObject&&) = delete;

    // Anonymous shared backing, zeroed before exposure. POSIX backing is sparse;
    // PS5 direct memory is allocated here. Large guest heaps use SharedHeap.
    // Closing the object does not invalidate views.
    static SharedObject create(size_t size);
    explicit operator bool() const { return fd_ >= 0 || static_cast<bool>(native_); }
    size_t size() const { return size_; }
    Mapping map(size_t offset, size_t size, Access access, uintptr_t address = 0) const;

private:
    SharedObject(int fd, size_t size);
    int fd_ = -1;
    size_t size_ = 0;
    std::unique_ptr<NativeSharedObject> native_;
};

} // namespace nxrt::host_vm
