// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/services.h"
namespace nxrt::services {
ServiceEvent::ServiceEvent(bool signaled) : st_(std::make_shared<kernel::EventState>()) { st_->signaled = signaled; }
uint32_t ServiceEvent::new_handle() const {
    auto event = std::make_shared<kernel::KEvent>();
    event->state = st_;
    event->writable = false;
    return kernel::handle_add(std::move(event));
}
void ServiceEvent::signal() { st_->signaled = true; kernel::notify_waitable_changed(); }
void ServiceEvent::clear() { st_->signaled = false; }
bool ServiceEvent::signaled() const { return st_->signaled; }
ServiceSharedMemory::ServiceSharedMemory(uint64_t size, const char* name)
    : obj_(kernel::shared_memory_create(size, name, 1)) {}
uint32_t ServiceSharedMemory::new_handle() const { return obj_ ? kernel::handle_add(obj_) : 0; }
}
