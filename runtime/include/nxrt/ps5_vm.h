// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse native PS5 reservations and shared direct-memory backing. Guest page
// policy is deliberately separate; every operation here uses host pages.
// Callers serialize mutations of each reservation and backing handle.
#pragma once
#include "nxrt/host_vm.h"
#include <memory>
#include <vector>

namespace nxrt::ps5_vm {
struct Backing;
class Reservation;
class DirectMemory {
public:
    static DirectMemory allocate(size_t size);
    DirectMemory() = default;
    explicit operator bool() const { return static_cast<bool>(backing_); }
    size_t size() const;
private:
    friend class Reservation;
    std::shared_ptr<Backing> backing_;
};

class Reservation {
public:
    static Reservation reserve(size_t size, uintptr_t address = 0);
    Reservation() = default;
    ~Reservation();
    Reservation(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation& operator=(Reservation&&) = delete;
    explicit operator bool() const { return size_ != 0; }
    void* data() const { return address_; }
    size_t size() const { return size_; }
    // Bind only previously unbacked portions of this reservation. Views keep
    // backing alive independently of DirectMemory handles. Contents are not
    // initialized here; the guest heap must zero newly allocated pages.
    bool map(size_t offset, const DirectMemory& backing, size_t backing_offset,
             size_t size, host_vm::Access access);
    bool protect(size_t offset, size_t size, host_vm::Access access);
    // Restore an inaccessible reservation without opening an address-space
    // hole. Backing remains alive wherever another view still references it.
    bool discard(size_t offset, size_t size);
    bool reset();
private:
    struct View { size_t offset, size; std::shared_ptr<Backing> backing; };
    void* address_ = nullptr;
    size_t size_ = 0;
    std::vector<View> views_;
};
} // namespace nxrt::ps5_vm
