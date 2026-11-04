// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only add-on inventory for a port with no installed DLC. Wire layouts
// follow misc.cpp and Eden's addon_content_manager.cpp. The empty inventory
// has no lost mounts; mounting and purchasing content remain unsupported.
#include "services_internal.h"
#include "nxrt/kernel.h"
#include "nxrt/subpage.h"
#include <mutex>

namespace nxrt::services {
namespace {
using ipc::Request;
using ipc::Response;
using ipc::Service;
struct State {
    const std::shared_ptr<kernel::KProcess> process = kernel::current_process();
    std::shared_ptr<kernel::EventState> changed = std::make_shared<kernel::EventState>();
    std::shared_ptr<kernel::EventState> purchased = std::make_shared<kernel::EventState>();
};
std::mutex mutex;
std::shared_ptr<State> state;
class AddOnService final : public Service {
    const std::shared_ptr<State> state_;
public:
    explicit AddOnService(std::shared_ptr<State> state) : Service("IAddOnContentManager"), state_(std::move(state)) {}
    uint32_t handle(Request& req, Response& rsp) override {
        if (state_->process != kernel::current_process()) return kernel::kInvalidHandle;
        return Service::handle(req, rsp);
    }
};
uint32_t event(const std::shared_ptr<kernel::EventState>& state, Response& rsp) {
    auto object = std::make_shared<kernel::KEvent>(); object->state = state;
    const auto handle = kernel::handle_add(object);
    if (!handle) return kernel::kOutOfHandles;
    rsp.out_copy.push_back(handle); return 0;
}
std::shared_ptr<Service> make_aoc() {
    std::shared_ptr<State> shared;
    { std::lock_guard lock(mutex); if (!state) state = std::make_shared<State>(); shared = state; }
    auto service = std::make_shared<AddOnService>(shared);
    const auto setup = [shared](Service& s) {
        for (const auto command : {0u, 2u})
            s.add_cmd(command, "CountAddOnContent", [](Request&, Response& rsp) -> uint32_t {
                rsp.push<int32_t>(0); return 0;
            });
        for (const auto command : {1u, 3u})
            s.add_cmd(command, "ListAddOnContent", [](Request& req, Response& rsp) -> uint32_t {
                const auto buffer = req.out_buf(0);
                if (buffer.size && !mem::SubpageMap::allows(buffer.addr, buffer.size, 2))
                    return kernel::kInvalidMemoryState;
                // No entries are written; only the returned count is meaningful.
                rsp.push<int32_t>(0); return 0;
            });
        s.add_cmd(4, "GetAddOnContentBaseIdByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            if (req.in_size < 8) return kernel::kInvalidCombination;
            const auto id = req.arg<uint64_t>(0);
            if (id > UINT64_MAX - 0x1000) return kernel::kOutOfRange;
            rsp.push<uint64_t>(id + 0x1000); return 0;
        });
        s.add_cmd(5, "GetAddOnContentBaseId", [](Request&, Response& rsp) -> uint32_t {
            const auto id = program_id_now();
            if (id > UINT64_MAX - 0x1000) return kernel::kOutOfRange;
            rsp.push<uint64_t>(id + 0x1000); return 0;
        });
        for (const auto command : {8u, 10u})
            s.add_cmd(command, "GetAddOnContentListChangedEvent", [shared](Request&, Response& rsp) {
                return event(shared->changed, rsp);
            });
        s.add_cmd(9, "GetAddOnContentLostErrorCode", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0); return 0;
        });
        s.add_cmd(13, "IsAddOnContentMountedForDebug", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0); return 0;
        });
        s.add_cmd(50, "CheckAddOnContentMountStatus", [](Request& req, Response&) -> uint32_t {
            if (!req.in_copy.empty() || !req.in_move.empty()) return kernel::kInvalidHandle;
            if (!req.in_services.empty() || !req.a.empty() || !req.b.empty() || !req.w.empty() ||
                !req.x.empty() || !req.c.empty()) return kernel::kInvalidCombination;
            // No inputs/outputs. With no installed or mounted add-ons there is
            // no lost mount to report. This does not mount or authorize content.
            return 0;
        });
        for (const auto command : {100u, 101u})
            s.add_cmd(command, "CreatePurchaseEventManager", [shared](Request&, Response& rsp) -> uint32_t {
                rsp.push_object(make_iface("IPurchaseEventManager", [shared](Service& manager) {
                    manager.add_cmd(2, "GetPurchasedEventReadableHandle", [shared](Request&, Response& out) {
                        return event(shared->purchased, out);
                    });
                    for (const auto cmd : {3u, 4u})
                        manager.add_cmd(cmd, "PopPurchasedProductInfo", [](Request&, Response&) -> uint32_t {
                            return res(164, 400);
                        });
                })); return 0;
            });
        for (const auto command : {6u, 7u, 11u, 12u, 110u, 200u})
            s.add_cmd(command, "AddOnOperationUnavailable", [](Request&, Response&) -> uint32_t {
                return kernel::kNotImplemented;
            });
    };
    setup(*service); return service;
}
}
void register_aoc() { add("aoc:u", make_aoc); }
void aoc_reset() { std::lock_guard lock(mutex); state.reset(); }
}
