// SPDX-License-Identifier: GPL-3.0-or-later
// GSV user-session bootstrap. See ps5/GRAPHICS_SUPERVISOR.md for the wire evidence.
#include "gsv_ps5.h"
#include "nxrt/ipc.h"
#include <mutex>
#include <optional>

namespace nxrt::services {
namespace {
namespace k = kernel;
struct RemoteBoot {
    uint64_t resource_user_id, version;
};
struct Context {
    std::shared_ptr<k::KProcess> process;
    RemoteBoot boot;
};
class GraphicsSupervisor final : public ipc::Service {
    std::mutex mutex;
    const std::shared_ptr<k::KProcess> owner;
    std::optional<RemoteBoot> boot;
    std::shared_ptr<k::KProcess> bound_process;
    std::unique_ptr<Context> context;

    bool process_matches(const ipc::Request& req) const {
        return req.in_copy.size() == 1 && req.in_move.empty() &&
            k::handle_as<k::KProcess>(req.in_copy[0]) == owner;
    }
public:
    explicit GraphicsSupervisor(std::shared_ptr<k::KProcess> process)
        : Service("GraphicsSupervisor"), owner(std::move(process)) {}
    uint32_t handle(ipc::Request& req, ipc::Response& rsp) override {
        if (!req.tipc || !req.a.empty() || !req.b.empty() || !req.w.empty() ||
            !req.x.empty() || !req.c.empty() || !req.in_services.empty()) return k::kInvalidCombination;
        std::lock_guard lock(mutex);
        switch (req.cmd) {
        case 4: { // BootWithRemoteMemory: aruid, version, three reserved u64s, usage.
            if (req.in_size != 48) return k::kInvalidSize;
            if (!process_matches(req)) return k::kInvalidHandle;
            if (req.arg<uint64_t>(0) != owner->id) return k::kInvalidCombination;
            const auto version = req.arg<uint64_t>(8);
            if (version != 0x8000000000000005ull) return k::kNotImplemented;
            for (size_t offset : {16u, 24u, 32u})
                if (req.arg<uint64_t>(offset)) return k::kInvalidCombination;
            // This port has not donated remote GPU allocations. Do not accept
            // pre-existing usage until its ownership/accounting is implemented.
            if (req.arg<uint64_t>(40)) return k::kNotImplemented;
            if (boot) return k::kInvalidState;
            boot = RemoteBoot{owner->id, version};
            return k::kOk;
        }
        case 11: // Retain the process object, not the transient IPC copy handle.
            if (req.in_size) return k::kInvalidSize;
            if (!process_matches(req)) return k::kInvalidHandle;
            if (!boot || bound_process) return k::kInvalidState;
            bound_process = k::handle_as<k::KProcess>(req.in_copy[0]);
            return k::kOk;
        case 101:
            if (req.in_size) return k::kInvalidSize;
            if (!req.in_copy.empty() || !req.in_move.empty()) return k::kInvalidHandle;
            if (!bound_process || context) return k::kInvalidState;
            context = std::make_unique<Context>(Context{bound_process, *boot});
            return k::kOk;
        default:
            // Record the unsupported operation, but never let a configured stub
            // result turn unbacked GPU/display/syncpoint work into success.
            (void)Service::handle(req, rsp);
            return k::kNotImplemented;
        }
    }
};
}
std::shared_ptr<ipc::Service> make_graphics_supervisor(std::shared_ptr<kernel::KProcess> owner) {
    if (!owner) return {};
    return std::make_shared<GraphicsSupervisor>(std::move(owner));
}
}
