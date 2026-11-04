// SPDX-License-Identifier: GPL-3.0-or-later
// Offline account services for the native runtime. Wire layouts follow acc.cpp,
// libnx acc.c/acc.h and the supplied SDK's application-service proxies. No PS5
// accounts, online identities, entitlement records or persistent files are made.
#include "services_internal.h"
#include "nxrt/acc_ps5.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "nxrt/subpage.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <mutex>

namespace nxrt::ipc::detail { uint64_t note_unhandled(Service&, uint32_t); }
namespace nxrt::services {
namespace {
using ipc::Request; using ipc::Response; using ipc::Service;
using Ptr = std::shared_ptr<Service>;
constexpr uint32_t invalid_uid = res(124, 20), absent_user = res(124, 100);
constexpr uint32_t invalid_array = res(124, 32), already_initialized = res(124, 41);
constexpr uint32_t registration_required = res(124, 200);
struct Uid {
    uint64_t hi, lo;
    bool operator==(const Uid&) const = default;
};
static_assert(sizeof(Uid) == 16);
struct State {
    const std::shared_ptr<kernel::KProcess> process = kernel::current_process();
    Uid uid{};
    std::array<uint8_t, 0x38> base{};
    std::mutex mutex;
    bool initialized = false;
    State() {
        const auto defaults = title_defaults();
        uid = {defaults.user_uid_hi, defaults.user_uid_lo};
        const auto configured = config_get_str("account", "uid");
        if (!configured.empty()) {
            Uid parsed{};
            const auto parse = [&](size_t offset, uint64_t& word) {
                const char* first = configured.data() + offset;
                const auto result = std::from_chars(first, first + 16, word, 16);
                return result.ec == std::errc{} && result.ptr == first + 16;
            };
            if (configured.size() == 32 && parse(0, parsed.hi) && parse(16, parsed.lo) && (parsed.hi | parsed.lo))
                uid = parsed;
            else NXLOG_W("acc", "Ignoring invalid account UID; using the local profile default");
        }
        if (!(uid.hi | uid.lo)) uid = {1, 0x6e78727475736572ull};
        std::memcpy(base.data(), &uid, sizeof uid);
        constexpr uint64_t timestamp = 1546300800; // stable local-profile creation timestamp
        std::memcpy(base.data() + 0x10, &timestamp, sizeof timestamp);
        const auto name = config_get_str("account", "nickname", defaults.user_nickname.c_str());
        size_t count = std::min<size_t>(name.size(), 32);
        if (count < name.size())
            while (count && (static_cast<unsigned char>(name[count]) & 0xc0) == 0x80) --count;
        std::memcpy(base.data() + 0x18, name.data(), count);
    }
};
std::mutex state_mutex;
std::shared_ptr<State> state;

class AccountService final : public Service {
    const std::shared_ptr<State> state_;
public:
    AccountService(const char* name, std::shared_ptr<State> state) : Service(name), state_(std::move(state)) {}
    uint32_t handle(Request& req, Response& rsp) override {
        if (state_->process != kernel::current_process()) return kernel::kInvalidHandle;
        if (cmds_.contains(req.cmd)) return Service::handle(req, rsp);
        if (ipc::detail::note_unhandled(*this, req.cmd) == 1)
            NXLOG_W("acc", "%s cmd %u is not ported", name().c_str(), req.cmd);
        return kernel::kNotImplemented;
    }
};
uint32_t request(const Request& req, size_t bytes, bool output = false) {
    if (!req.in_copy.empty() || !req.in_move.empty()) return kernel::kInvalidHandle;
    if (!req.in_services.empty() || !req.a.empty() || !req.x.empty() || !req.w.empty())
        return kernel::kInvalidCombination;
    if ((!output && (!req.b.empty() || !req.c.empty())) || req.b.size() > 1 || req.c.size() > 1 ||
        (!req.b.empty() && req.b[0].addr && !req.c.empty() && req.c[0].addr)) return kernel::kInvalidCombination;
    if (req.in_size < bytes || (bytes && !req.in)) return kernel::kInvalidSize;
    return 0;
}
uint32_t output(const ipc::Buffer& buffer, const void* data, size_t size) {
    if (!buffer.addr || buffer.size < size) return invalid_array;
    if (!mem::SubpageMap::allows(buffer.addr, size, 2)) return kernel::kInvalidMemoryState;
    std::memcpy(reinterpret_cast<void*>(buffer.addr), data, size); return 0;
}
uint32_t known_user(const Request& req, const State& state) {
    if (const auto rc = request(req, sizeof(Uid))) return rc;
    const auto uid = req.arg<Uid>(0);
    if (!(uid.hi | uid.lo)) return invalid_uid;
    return uid == state.uid ? 0 : absent_user;
}
uint32_t user_list(const Request& req, const State& state) {
    if (const auto rc = request(req, 0, true)) return rc;
    const auto buffer = req.out_buf(0);
    if (buffer.size < sizeof(Uid) || buffer.size % sizeof(Uid)) return invalid_array;
    // Both published account APIs use fixed user arrays (8 or 10 entries).
    // Cap writes even when a caller advertises an excessively large buffer.
    std::array<uint8_t, 0xa0> bytes{};
    std::memcpy(bytes.data(), &state.uid, sizeof state.uid);
    return output(buffer, bytes.data(), std::min<uint64_t>(bytes.size(), buffer.size));
}
Ptr profile_service(const std::shared_ptr<State>& state) {
    auto s = std::make_shared<AccountService>("IProfile", state);
    s->add_cmd(0, "Get", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0, true)) return rc;
        std::array<uint8_t, 0x80> data{};
        // Icon 1 is a stock account icon; icon 0 would claim a Mii is present.
        data[4] = 1;
        if (const auto rc = output(req.out_buf(0), data.data(), data.size())) return rc;
        rsp.out.assign(state->base.begin(), state->base.end()); return 0;
    });
    s->add_cmd(1, "GetBase", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.out.assign(state->base.begin(), state->base.end()); return 0;
    });
    for (auto command : {10u, 11u}) {
        s->add_cmd(command, command == 10 ? "GetImageSize" : "LoadImage", [command](Request& req, Response& rsp) -> uint32_t {
            if (const auto rc = request(req, 0, command == 11)) return rc;
            // There is no encoded avatar image in this runtime.
            rsp.push<uint32_t>(0); return 0;
        });
    }
    return s;
}
Ptr async_service(const std::shared_ptr<State>& state, bool license) {
    auto s = std::make_shared<AccountService>(license ? "IAsyncNetworkServiceLicenseKindContext" : "IAsyncContext", state);
    auto done = std::make_shared<kernel::EventState>(); done->signaled = true;
    s->add_cmd(0, "GetSystemEvent", [done](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        auto event = std::make_shared<kernel::KEvent>(); event->state = done;
        const auto handle = kernel::handle_add(event);
        if (!handle) return kernel::kOutOfHandles;
        rsp.out_copy.push_back(handle); return 0;
    });
    s->add_cmd(1, "Cancel", [](Request& req, Response&) { return request(req, 0); });
    s->add_cmd(2, "HasDone", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<uint8_t>(1); return 0;
    });
    s->add_cmd(3, "GetResult", [](Request& req, Response&) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        return registration_required;
    });
    if (license) s->add_cmd(100, "GetNetworkServiceLicenseKind", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<uint32_t>(0); return 0; // no online subscription
    });
    return s;
}
Ptr manager_service(const std::shared_ptr<State>& state) {
    auto s = std::make_shared<AccountService>("IManagerForApplication", state);
    for (auto command : {0u, 1u, 3u, 4u}) {
        s->add_cmd(command, "NetworkAccountUnavailable", [command](Request& req, Response&) -> uint32_t {
            if (const auto rc = request(req, 0, command == 3 || command == 4)) return rc;
            // CheckAvailability returns only a Result, never a boolean or a
            // synthetic network account ID. The local user is not linked.
            return registration_required;
        });
    }
    for (auto command : {2u, 170u}) {
        s->add_cmd(command, command == 2 ? "EnsureIdTokenCacheAsync" : "LoadNetworkServiceLicenseKindAsync",
            [state, command](Request& req, Response& rsp) -> uint32_t {
                if (const auto rc = request(req, 0)) return rc;
                rsp.push_object(async_service(state, command == 170)); return 0;
            });
    }
    return s;
}
Ptr account_service(const std::shared_ptr<State>& state, bool system) {
    auto s = std::make_shared<AccountService>(system ? "acc:u1" : "acc:u0", state);
    s->add_cmd(0, "GetUserCount", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<int32_t>(1); return 0;
    });
    s->add_cmd(1, "GetUserExistence", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, sizeof(Uid))) return rc;
        if (const auto uid = req.arg<Uid>(0); !(uid.hi | uid.lo)) return invalid_uid;
        rsp.push<uint8_t>(req.arg<Uid>(0) == state->uid); return 0;
    });
    for (auto command : {2u, 3u}) s->add_cmd(command, command == 2 ? "ListAllUsers" : "ListOpenUsers",
        [state](Request& req, Response&) { return user_list(req, *state); });
    s->add_cmd(4, "GetLastOpenedUser", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push(state->uid); return 0;
    });
    s->add_cmd(5, "GetProfile", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = known_user(req, *state)) return rc;
        rsp.push_object(profile_service(state)); return 0;
    });
    s->add_cmd(50, "IsUserRegistrationRequestPermitted", [state](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 8)) return rc;
        if (!req.pid || req.pid != state->process->id) return kernel::kInvalidHandle;
        rsp.push<uint8_t>(0); return 0;
    });
    for (auto command : {51u, 52u}) s->add_cmd(command, "TrySelectUserWithoutInteraction",
        [state](Request& req, Response& rsp) -> uint32_t {
            if (const auto rc = request(req, 1)) return rc;
            const auto network_required = req.arg<uint8_t>(0);
            if (network_required > 1) return kernel::kOutOfRange;
            rsp.push(network_required ? Uid{} : state->uid); return 0;
        });
    s->add_cmd(system ? 140 : 141, "ListQualifiedUsers", [state](Request& req, Response&) { return user_list(req, *state); });
    if (system) return s;
    for (auto command : {100u, 140u, 160u}) s->add_cmd(command, "InitializeApplicationInfo",
        [state](Request& req, Response&) -> uint32_t {
            if (const auto rc = request(req, 8)) return rc;
            if (!req.pid || req.pid != state->process->id) return kernel::kInvalidHandle;
            std::lock_guard lock(state->mutex);
            if (state->initialized) return already_initialized;
            state->initialized = true; return 0;
        });
    for (auto command : {101u, 130u}) s->add_cmd(command, command == 101 ? "GetBaasAccountManagerForApplication" : "LoadOpenContext",
        [state](Request& req, Response& rsp) -> uint32_t {
            if (const auto rc = known_user(req, *state)) return rc;
            std::lock_guard lock(state->mutex);
            if (!state->initialized) return kernel::kInvalidState;
            rsp.push_object(manager_service(state)); return 0;
        });
    s->add_cmd(131, "ListOpenContextStoredUsers", [state](Request& req, Response&) { return user_list(req, *state); });
    s->add_cmd(150, "IsUserAccountSwitchLocked", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<uint8_t>(1); return 0; // fixed single-profile native frontend
    });
    return s;
}
}
void register_acc() {
    std::lock_guard lock(state_mutex);
    if (!state) state = std::make_shared<State>();
    add("acc:u0", [snapshot = state] { return account_service(snapshot, false); });
    add("acc:u1", [snapshot = state] { return account_service(snapshot, true); });
}
void acc_reset() { std::lock_guard lock(state_mutex); state.reset(); }
namespace accapi {
bool launch_user(uint64_t* hi, uint64_t* lo) {
    std::lock_guard lock(state_mutex);
    if (!state || state->process != kernel::current_process()) return false;
    if (hi) *hi = state->uid.hi;
    if (lo) *lo = state->uid.lo;
    return true;
}
void user_uid(uint64_t* hi, uint64_t* lo) {
    if (hi) *hi = 0;
    if (lo) *lo = 0;
    launch_user(hi, lo);
}
}
}
