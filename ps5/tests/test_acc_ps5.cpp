// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/acc_ps5.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "trace_ipc_reply.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

NXTEST_GROUP(acc_ps5)
namespace {
namespace ipc = nxrt::ipc; namespace k = nxrt::kernel; namespace mem = nxrt::mem;
namespace acc = nxrt::services;
using Ptr = std::shared_ptr<ipc::Service>;
using Uid = std::array<uint64_t, 2>;
constexpr Uid local{1, 0x6e78727475736572ull};
struct Answer { uint32_t result = 0; ipc::Response response; };
Answer call(const Ptr& service, uint32_t command, const void* input = nullptr, size_t size = 0,
            ipc::Buffer buffer = {}, uint64_t pid = 0) {
    ipc::Request req; req.cmd = command; req.in = static_cast<const uint8_t*>(input); req.in_size = size; req.pid = pid;
    if (buffer.addr || buffer.size) req.c.push_back(buffer);
    Answer answer; answer.result = service->handle(req, answer.response); return answer;
}
template<class T> T value(const Answer& answer, size_t offset = 0) {
    if (answer.result || answer.response.out.size() < offset + sizeof(T)) throw std::runtime_error("missing account reply");
    T result{}; std::memcpy(&result, answer.response.out.data() + offset, sizeof result); return result;
}
Ptr child(const Answer& answer) {
    return !answer.result && answer.response.out_objects.size() == 1 ? answer.response.out_objects[0] : nullptr;
}
struct Fixture {
    const nxrt::game::Info* previous = nxrt::game::current();
    Ptr service; uint64_t data = 0;
    Fixture(const char* config = "") {
        acc::acc_reset(); k::kernel_reset(); ipc::registry_reset(); mem::shutdown();
        nxrt::game::set_current(nullptr); nxrt::config_load_string(config);
        if (!mem::init({.heap_reserve = 64ull << 20}) || !(data = mem::alloc_kernel(0x8000)))
            throw std::runtime_error("cannot allocate account test memory");
        acc::register_acc(); service = ipc::make_service("acc:u0");
    }
    ~Fixture() {
        service.reset(); acc::acc_reset(); ipc::registry_reset(); k::kernel_reset(); mem::shutdown();
        nxrt::game::set_current(previous); nxrt::config_clear();
    }
    Answer initialize(uint32_t command = 160) {
        const uint64_t placeholder = 0;
        return call(service, command, &placeholder, 8, {}, k::current_process()->id);
    }
    Ptr manager() {
        if (initialize().result) return nullptr;
        return child(call(service, 101, local.data(), 16));
    }
};
}

NXTEST(account_local_identity_is_consistent_across_selection_profile_and_user_lists) {
    Fixture f; CHECK(!f.service->is_stub()); CHECK_EQ(value<int32_t>(call(f.service, 0)), 1);
    CHECK(!ipc::has_service("acc:su") && !ipc::has_service("acc:aa"));
    CHECK(value<Uid>(call(f.service, 4)) == local);
    CHECK_EQ(value<uint8_t>(call(f.service, 1, local.data(), 16)), 1);
    const Uid absent{2, 3}; CHECK_EQ(value<uint8_t>(call(f.service, 1, absent.data(), 16)), 0);
    const Uid zero{}; CHECK_EQ(call(f.service, 1, zero.data(), 16).result, k::make_result(124, 20));
    for (auto command : {51u, 52u}) {
        uint8_t network = 0; CHECK(value<Uid>(call(f.service, command, &network, 1)) == local);
        network = 1; CHECK(value<Uid>(call(f.service, command, &network, 1)) == zero);
        network = 2; CHECK_EQ(call(f.service, command, &network, 1).result, k::kOutOfRange);
    }
    for (auto command : {2u, 3u, 131u, 141u}) {
        std::memset(reinterpret_cast<void*>(f.data), 0xff, 0x100);
        CHECK_EQ(call(f.service, command, nullptr, 0, {f.data, 0x80}).result, 0u);
        CHECK(std::memcmp(reinterpret_cast<void*>(f.data), local.data(), 16) == 0);
        const auto bytes = reinterpret_cast<uint8_t*>(f.data);
        CHECK(std::all_of(bytes + 16, bytes + 0x80, [](auto v) { return v == 0; }));
        CHECK_EQ(bytes[0x80], 0xff);
    }
    auto sys = ipc::make_service("acc:u1");
    CHECK_EQ(call(sys, 140, nullptr, 0, {f.data, 0xa0}).result, 0u);
    CHECK_EQ(call(sys, 160).result, k::kNotImplemented);
}
NXTEST(account_initialization_checks_pid_and_shares_state_across_application_sessions) {
    Fixture f; const uint64_t placeholder = 0;
    CHECK_EQ(call(f.service, 160).result, k::kInvalidSize);
    CHECK_EQ(call(f.service, 160, &placeholder, 8).result, k::kInvalidHandle);
    CHECK_EQ(call(f.service, 160, &placeholder, 8, {}, k::current_process()->id + 1).result, k::kInvalidHandle);
    CHECK_EQ(call(f.service, 101, local.data(), 16).result, k::kInvalidState);
    for (auto command : {100u, 140u, 160u}) {
        acc::acc_reset(); acc::register_acc(); f.service = ipc::make_service("acc:u0");
        CHECK_EQ(f.initialize(command).result, 0u);
        auto second = ipc::make_service("acc:u0");
        CHECK_EQ(call(second, command, &placeholder, 8, {}, k::current_process()->id).result, k::make_result(124, 41));
        CHECK(child(call(second, 101, local.data(), 16)));
        CHECK(child(call(second, 130, local.data(), 16)));
    }
    CHECK_EQ(value<uint8_t>(call(f.service, 50, &placeholder, 8, {}, k::current_process()->id)), 0);
    CHECK_EQ(value<uint8_t>(call(f.service, 150)), 1);
}
NXTEST(account_profile_layout_matches_the_sdk_and_empty_images_leave_buffers_untouched) {
    Fixture f; auto profile = child(call(f.service, 5, local.data(), 16)); REQUIRE(profile);
    auto base = call(profile, 1); REQUIRE(base.response.out.size() == 0x38);
    CHECK(value<Uid>(base) == local); CHECK_EQ(value<uint64_t>(base, 16), 1546300800ull);
    CHECK(std::memcmp(base.response.out.data() + 24, "Player\0", 7) == 0);
    std::memset(reinterpret_cast<void*>(f.data), 0xff, 0x100);
    auto full = call(profile, 0, nullptr, 0, {f.data, 0x80}); CHECK_EQ(full.result, 0u);
    CHECK(full.response.out == base.response.out);
    const auto bytes = reinterpret_cast<const uint8_t*>(f.data);
    for (size_t i = 0; i < 0x80; ++i) CHECK_EQ(bytes[i], i == 4 ? 1 : 0);
    CHECK_EQ(bytes[0x80], 0xff);
    CHECK_EQ(value<uint32_t>(call(profile, 10)), 0u);
    std::memset(reinterpret_cast<void*>(f.data), 0x73, 0x100);
    CHECK_EQ(value<uint32_t>(call(profile, 11, nullptr, 0, {f.data, 0x100})), 0u);
    CHECK(std::all_of(bytes, bytes + 0x100, [](auto v) { return v == 0x73; }));
    Uid unknown{7, 8}; CHECK_EQ(call(f.service, 5, unknown.data(), 16).result, k::make_result(124, 100));
    unknown = {}; CHECK_EQ(call(f.service, 5, unknown.data(), 16).result, k::make_result(124, 20));
}
NXTEST(account_rejects_short_foreign_and_unwritable_buffers_before_writing_any_data) {
    Fixture f; auto profile = child(call(f.service, 5, local.data(), 16)); REQUIRE(profile);
    std::memset(reinterpret_cast<void*>(f.data), 0xa5, 0x100);
    CHECK_EQ(call(profile, 0, nullptr, 0, {f.data, 0x7f}).result, k::make_result(124, 32));
    CHECK_EQ(call(f.service, 2, nullptr, 0, {f.data, 17}).result, k::make_result(124, 32));
    CHECK_EQ(call(f.service, 2).result, k::make_result(124, 32));
    CHECK_EQ(call(f.service, 5, local.data(), 15).result, k::kInvalidSize);
    CHECK_EQ(*reinterpret_cast<const uint8_t*>(f.data), 0xa5);
    REQUIRE(mem::protect(f.data + 0x1000, 0x1000, mem::Prot::R));
    CHECK_EQ(call(profile, 0, nullptr, 0, {f.data + 0xfc0, 0x80}).result, k::kInvalidMemoryState);
    CHECK_EQ(call(f.service, 2, nullptr, 0, {UINT64_MAX - 7, 0x80}).result, k::kInvalidMemoryState);
    ipc::Request req; req.cmd = 0; req.in_copy = {k::kPseudoCurrentProcess}; ipc::Response rsp;
    CHECK_EQ(f.service->handle(req, rsp), k::kInvalidHandle); CHECK(rsp.out.empty());
    req.in_copy.clear(); req.a.push_back({f.data, 16});
    CHECK_EQ(f.service->handle(req, rsp), k::kInvalidCombination);
    for (auto command : {6u, 110u, 111u, 999u}) CHECK_EQ(call(f.service, command).result, k::kNotImplemented);
}
NXTEST(account_configuration_is_snapshotted_and_old_profiles_survive_account_reset) {
    Fixture f("[account]\nuid=0123456789abcdef1122334455667788\nnickname=Local Player\n");
    const Uid expected{0x0123456789abcdef, 0x1122334455667788};
    CHECK(value<Uid>(call(f.service, 4)) == expected);
    auto profile = child(call(f.service, 5, expected.data(), 16)); REQUIRE(profile);
    nxrt::config_set("account", "uid", "00000000000000000000000000000000");
    nxrt::config_set("account", "nickname", "Replacement");
    CHECK(value<Uid>(call(f.service, 4)) == expected);
    acc::acc_reset(); acc::register_acc(); const auto fresh = ipc::make_service("acc:u0");
    CHECK(value<Uid>(call(fresh, 4)) == local);
    const auto old_base = call(profile, 1); CHECK(value<Uid>(old_base) == expected);
    CHECK(std::memcmp(old_base.response.out.data() + 24, "Local Player", 12) == 0);
    uint64_t hi = 0, lo = 0; REQUIRE(acc::accapi::launch_user(&hi, &lo)); CHECK_EQ(hi, local[0]); CHECK_EQ(lo, local[1]);
    acc::acc_reset(); CHECK(!acc::accapi::launch_user(&hi, &lo));
}
NXTEST(account_handles_do_not_cross_guest_process_boundaries) {
    Fixture f; auto profile = child(call(f.service, 5, local.data(), 16)); REQUIRE(profile);
    auto manager = f.manager(); REQUIRE(manager); auto async = child(call(manager, 2)); REQUIRE(async);
    k::kernel_reset();
    for (const auto& service : {f.service, profile, manager, async})
        CHECK_EQ(call(service, 0).result, k::kInvalidHandle);
    CHECK(!acc::accapi::launch_user(nullptr, nullptr));
}
NXTEST(account_offline_network_calls_never_fabricate_ids_tokens_or_online_entitlements) {
    Fixture f; auto manager = f.manager(); REQUIRE(manager);
    for (auto command : {0u, 1u, 3u, 4u}) {
        const auto answer = call(manager, command); CHECK_EQ(answer.result, k::make_result(124, 200));
        CHECK(answer.response.out.empty() && answer.response.out_objects.empty() && answer.response.out_copy.empty());
        CHECK(!nxrt::diagnostic::stop_on_reply({true, answer.result}, manager->name(), command));
    }
    CHECK(nxrt::diagnostic::stop_on_reply({true, k::make_result(124, 200)}, manager->name(), 999));
    CHECK(nxrt::diagnostic::stop_on_reply({true, k::kNotImplemented}, manager->name(), 0));
    CHECK(nxrt::diagnostic::stop_on_reply({false, k::make_result(124, 200)}, manager->name(), 0));
    CHECK(nxrt::diagnostic::stop_on_reply({true, k::make_result(124, 200)}, "IProfile", 3));
    for (auto command : {2u, 170u}) {
        auto async = child(call(manager, command)); REQUIRE(async);
        CHECK_EQ(value<uint8_t>(call(async, 2)), 1);
        CHECK_EQ(call(async, 1).result, 0u); CHECK_EQ(call(async, 1).result, 0u);
        CHECK_EQ(call(async, 3).result, k::make_result(124, 200));
        CHECK(!nxrt::diagnostic::stop_on_reply({true, k::make_result(124, 200)}, async->name(), 3));
        if (command == 170) CHECK_EQ(value<uint32_t>(call(async, 100)), 0u);
        else CHECK_EQ(call(async, 100).result, k::kNotImplemented);
    }
}
NXTEST(account_async_completion_events_are_readable_shared_and_handle_exhaustion_is_reported) {
    Fixture f; auto manager = f.manager(); REQUIRE(manager); auto async = child(call(manager, 2)); REQUIRE(async);
    auto first = call(async, 0), second = call(async, 0); REQUIRE(!first.result && !second.result);
    REQUIRE(first.response.out_copy.size() == 1 && second.response.out_copy.size() == 1);
    const auto h1 = first.response.out_copy[0], h2 = second.response.out_copy[0];
    const auto e1 = k::handle_as<k::KEvent>(h1), e2 = k::handle_as<k::KEvent>(h2); REQUIRE(e1 && e2);
    CHECK(e1->state == e2->state); CHECK(e1->signaled());
    int32_t index = -1; CHECK_EQ(k::wait_synchronization(&h1, 1, 0, &index), 0u); CHECK_EQ(index, 0);
    k::handle_close(h1); CHECK(e2->signaled());
    auto other = child(call(manager, 2)); REQUIRE(other); auto third = call(other, 0); REQUIRE(!third.result);
    REQUIRE(third.response.out_copy.size() == 1);
    CHECK(k::handle_as<k::KEvent>(third.response.out_copy[0])->state != e2->state);
    std::vector<uint32_t> handles;
    while (auto h = k::handle_add(std::make_shared<k::KEvent>())) handles.push_back(h);
    const auto exhausted = call(async, 0); CHECK_EQ(exhausted.result, k::kOutOfHandles); CHECK(exhausted.response.out_copy.empty());
    for (auto h : handles) k::handle_close(h);
}
