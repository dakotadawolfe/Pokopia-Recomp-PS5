// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
namespace nxrt::ipc { class Service; }
namespace nxrt::kernel { struct KProcess; }
namespace nxrt::services {
std::shared_ptr<ipc::Service> make_graphics_supervisor(std::shared_ptr<kernel::KProcess> owner);
}
