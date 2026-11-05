// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/dispatch.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services_api.h"
#include "nxrt/ipc.h"
#include "nxrt/memory.h"
#include "../app/game_input.h"
#include "trace_ipc_reply.h"
#include <cstring>

namespace nxrt::services { void register_hid(); void hid_reset(); void hid_sample_once(); void hid_set_background_sampling(bool); void register_power(); void power_reset(); }
namespace nxrt::services { void register_network(); void network_reset(); }
NXTEST_GROUP(startup_dependencies)
namespace {
nxrt::ipc::Response invoke(const std::shared_ptr<nxrt::ipc::Service>& service, uint32_t cmd) {
    nxrt::ipc::Request req; req.cmd = cmd; nxrt::ipc::Response rsp;
    CHECK_EQ(service->handle(req, rsp), 0u); return rsp;
}
uint64_t read64(uint64_t address) { uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(address), 8); return v; }
struct Reset {
    Reset() { nxrt::modules_unload_all(); nxrt::hle_clear(); }
    ~Reset() { nxrt::modules_unload_all(); nxrt::hle_clear(); }
};
}
NXTEST(native_libc_imports_preserve_results_and_registers) {
    Reset reset;
    static const DynSym syms[] = {{"strlen", 0, 0}, {"memcmp", 0, 0}, {"memmove", 0, 0}, {"strnlen", 0, 0}};
    static const NxModule module = {"libc_test", 0, 0x7200000000ull, 0x7200001000ull, nullptr, 0, syms, 4, nullptr, 0, nullptr};
    REQUIRE(nxrt::nxmodule_register(nullptr, &module, nxrt::kAbiVersion).ok());
    nxrt::install_libc_hle();
    char bytes[16] = "pokopia"; Ctx c{}; c.x[0] = reinterpret_cast<uint64_t>(bytes);
    c.x[19] = 0xabcdef; c.in_import = "caller"; c.import_from = 0x1234;
    host_import(&c, NX_IMPORT_ID(0, 0)); CHECK_EQ(c.x[0], 7u); CHECK_EQ(c.x[19], 0xabcdefu);
    CHECK_EQ(std::string(c.in_import), std::string("caller")); CHECK_EQ(c.import_from, 0x1234u);
    c.x[0] = reinterpret_cast<uint64_t>("a"); c.x[1] = reinterpret_cast<uint64_t>("b"); c.x[2] = 1;
    host_import(&c, NX_IMPORT_ID(0, 1)); CHECK(static_cast<int64_t>(c.x[0]) < 0);
    c.x[0] = reinterpret_cast<uint64_t>(bytes + 1); c.x[1] = reinterpret_cast<uint64_t>(bytes); c.x[2] = 7;
    host_import(&c, NX_IMPORT_ID(0, 2)); CHECK_EQ(std::string(bytes, 8), std::string("ppokopia"));
    CHECK_EQ(c.x[0], reinterpret_cast<uint64_t>(bytes + 1));
    c.x[0] = reinterpret_cast<uint64_t>(bytes); c.x[1] = 3;
    host_import(&c, NX_IMPORT_ID(0, 3)); CHECK_EQ(c.x[0], 3u);
    const auto thunk = nxrt::hle_address("strlen"); REQUIRE(nxrt::hle_leaf_fn(thunk));
    nxrt::hle_register("strlen", +[](Ctx* c) { c->x[0] = 91; }); CHECK(!nxrt::hle_leaf_fn(thunk));
    c.x[0] = reinterpret_cast<uint64_t>(bytes); host_import(&c, NX_IMPORT_ID(0, 0)); CHECK_EQ(c.x[0], 91u);
}

NXTEST(native_controller_mapping_has_centered_sticks_and_releases_disconnect) {
    nxrt::app::PickerInput input; input.connected = true; input.native_buttons = 0x2000 | 0x400 | 8;
    auto p = nxrt::app::game_pad(input);
    CHECK(p.connected); CHECK_EQ(p.buttons, nxrt::hid::btn(nxrt::hid::kBtnA) | nxrt::hid::btn(nxrt::hid::kBtnL) | nxrt::hid::btn(nxrt::hid::kBtnPlus));
    CHECK_EQ(p.left.x, 0); CHECK_EQ(p.left.y, 0);
    input.lx = 0; input.ly = 255; input.rx = 255; input.ry = 0; p = nxrt::app::game_pad(input);
    CHECK_EQ(p.left.x, -32767); CHECK_EQ(p.left.y, -32767); CHECK_EQ(p.right.x, 32767); CHECK_EQ(p.right.y, 32767);
    input.connected = false; p = nxrt::app::game_pad(input); CHECK_EQ(p.buttons, 0u); CHECK(!p.connected); CHECK_EQ(p.left.x, 0);
}

NXTEST(hid_samples_reach_shared_guest_alias_and_survive_handle_close) {
    namespace k = nxrt::kernel; namespace svc = nxrt::services;
    k::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::mem::shutdown();
    REQUIRE(nxrt::mem::init({.heap_reserve = 64ull << 20}));
    struct Cleanup { ~Cleanup() { nxrt::services::hid_reset(); nxrt::hid::set_input_source(nullptr); nxrt::kernel::kernel_reset(); nxrt::ipc::registry_reset(); nxrt::mem::shutdown(); } } cleanup;
    svc::hid_reset(); svc::hid_set_background_sampling(false); svc::register_hid();
    auto source = std::make_shared<nxrt::hid::ManualInputSource>(); source->connect(0, nxrt::hid::NpadStyle::FullKey);
    source->press(0, nxrt::hid::btn(nxrt::hid::kBtnA)); nxrt::hid::set_input_source(source);
    auto hid = nxrt::ipc::make_service("hid"); auto resource = invoke(hid, 0); REQUIRE(resource.out_objects.size() == 1);
    auto reply = invoke(resource.out_objects[0], 0); REQUIRE(reply.out_copy.size() == 1);
    const auto handle = reply.out_copy[0]; const auto shared = k::handle_as<k::KSharedMemory>(handle);
    REQUIRE(shared && shared->size == 0x40000); const uint64_t alias = nxrt::mem::kAliasBase;
    REQUIRE(k::shared_memory_map(handle, alias, 0x40000, 1) == 0);
    invoke(hid, 103); svc::hid_sample_once();
    // Independent nn::hid NpadCommonState layout: FullKey ring at 0x9A00+0x28.
    const uint64_t ring = alias + 0x9a00 + 0x28;
    auto buttons = [&] { return read64(ring + 0x20 + read64(ring + 0x10) * 0x30 + 0x10); };
    CHECK_EQ(buttons(), nxrt::hid::btn(nxrt::hid::kBtnA));
    k::handle_close(handle); source->release(0, nxrt::hid::btn(nxrt::hid::kBtnA)); svc::hid_sample_once(); CHECK_EQ(buttons(), 0u);
    const uint32_t second = invoke(resource.out_objects[0], 0).out_copy[0];
    CHECK_EQ(k::shared_memory_unmap(second, alias, 0x40000), 0u); k::handle_close(second);
}

NXTEST(power_service_reports_docked_mode_without_clock_changes) {
    nxrt::services::power_reset(); nxrt::services::register_power();
    auto service = nxrt::ipc::make_service("apm"); auto reply = invoke(service, 1);
    REQUIRE(reply.out.size() >= 4); uint32_t mode; std::memcpy(&mode, reply.out.data(), 4); CHECK_EQ(mode, 1u);
    nxrt::services::power_reset(); nxrt::ipc::registry_reset();
}

NXTEST(offline_network_startup_exposes_objects_and_real_disconnected_results) {
    namespace ipc = nxrt::ipc;
    nxrt::services::network_reset(); ipc::registry_reset(); nxrt::services::register_network();
    auto creator = ipc::make_service("nifm:u"); auto general = invoke(creator, 5); REQUIRE(general.out_objects.size() == 1);
    ipc::Request req; ipc::Response rsp; req.cmd = 12;
    const uint32_t result = general.out_objects[0]->handle(req, rsp); CHECK_EQ(result, (300u << 9) | 110u);
    CHECK(!nxrt::diagnostic::stop_on_reply({true, result, 0}, general.out_objects[0]->name(), 12));
    CHECK(nxrt::diagnostic::stop_on_reply({true, result, 0}, "IFile", 12));
    CHECK(nxrt::diagnostic::stop_on_reply({true, nxrt::kernel::kStubService, 0}, general.out_objects[0]->name(), 99));
    CHECK(nxrt::diagnostic::stop_on_reply({false, result, 0}, general.out_objects[0]->name(), 12));
    const auto state = invoke(general.out_objects[0], 22); REQUIRE(state.out.size() == 1); CHECK_EQ(state.out[0], 0u);
    for (auto name : {"bcat:u", "friend:u", "ssl:s", "ldn:u"}) {
        auto root = ipc::make_service(name); CHECK(!root->is_stub());
        const auto child = invoke(root, 0); REQUIRE(child.out_objects.size() == 1); CHECK(!child.out_objects[0]->is_stub());
    }
    auto sockets = ipc::make_service("bsd:u"); const auto reply = invoke(sockets, 2);
    REQUIRE(reply.out.size() >= 8); int32_t status; uint32_t error;
    std::memcpy(&status, reply.out.data(), 4); std::memcpy(&error, reply.out.data() + 4, 4);
    CHECK_EQ(status, -1); CHECK_EQ(error, 100u);
    nxrt::kernel::kernel_reset(); nxrt::services::network_reset(); ipc::registry_reset();
}
