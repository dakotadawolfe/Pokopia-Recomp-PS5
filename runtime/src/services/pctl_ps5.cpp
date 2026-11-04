// SPDX-License-Identifier: GPL-3.0-or-later
// Application-side parental-control queries. The standalone guest profile has
// no configured restrictions. This does not modify PS5 shell/account policy.
// Command layouts follow network.cpp; administrative setters are unported.
#include "services_internal.h"
#include "nxrt/kernel.h"
#include <atomic>

namespace nxrt::services {
namespace {
using ipc::Request;
using ipc::Response;
using ipc::Service;
std::shared_ptr<Service> instance(bool initialized) {
    auto ready = std::make_shared<std::atomic<bool>>(initialized);
    return make_iface("nn::pctl::detail::ipc::IParentalControlService", [ready](Service& s) {
        s.add_cmd(1, "Initialize", [ready](Request&, Response&) -> uint32_t { *ready = true; return 0; });
        // Queries of this guest profile, not claims that a network/UI operation
        // or a PS5 permission change has completed.
        for (const auto command : {1006u, 1010u, 1031u, 1403u, 1453u})
            s.add_cmd(command, "GetDisabledRestrictionState", [ready](Request&, Response& rsp) -> uint32_t {
                if (!*ready) return kernel::kInvalidState;
                rsp.push<uint8_t>(0); return 0;
            });
        for (const auto command : {1012u, 1023u})
            s.add_cmd(command, "GetRestrictedFeatures", [ready](Request&, Response& rsp) -> uint32_t {
                if (!*ready) return kernel::kInvalidState;
                rsp.push<uint32_t>(0); return 0;
            });
        s.add_cmd(1001, "CheckFreeCommunicationPermission", [ready](Request&, Response&) -> uint32_t {
            return *ready ? 0 : kernel::kInvalidState;
        });
        s.add_cmd(1018, "IsFreeCommunicationAvailable", [ready](Request&, Response& rsp) -> uint32_t {
            if (!*ready) return kernel::kInvalidState;
            rsp.push<uint8_t>(1); return 0;
        });
    });
}
std::shared_ptr<Service> factory() {
    return make_iface("nn::pctl::detail::ipc::IParentalControlServiceFactory", [](Service& s) {
        for (const auto command : {0u, 1u})
            s.add_cmd(command, "CreateService", [command](Request& req, Response& rsp) -> uint32_t {
                if (!req.pid || req.pid != kernel::current_process()->id) return kernel::kInvalidHandle;
                rsp.push_object(instance(command == 0)); return 0;
            });
    });
}
}
void register_pctl() {
    for (const auto name : {"pctl", "pctl:s", "pctl:r", "pctl:a"}) add(name, factory);
}
}
