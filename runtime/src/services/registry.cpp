// SPDX-License-Identifier: GPL-3.0-or-later
#include "services_internal.h"
#include <mutex>
#include <set>

namespace nxrt::services {
namespace {
std::mutex g_mu;
std::set<std::string>& names() {
    static std::set<std::string> s;
    return s;
}
}  // namespace

void add(const std::string& name, ipc::ServiceFactory f) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        names().insert(name);
    }
    ipc::register_service(name, std::move(f));
}

std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<ipc::Service>(name);
    setup(*s);
    return s;
}

std::vector<std::string> registered_names() {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::vector<std::string>(names().begin(), names().end());
}

} // namespace nxrt::services
