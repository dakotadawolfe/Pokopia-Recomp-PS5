// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "trace_ipc_reply.h"
#include "nxrt/acc_ps5.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/memory.h"
#include "nxrt/services_api.h"
#include "../../runtime/src/services/fs_save_ps5.h"
#include "../../runtime/src/services/fs_volume.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

NXTEST_GROUP(save_ps5)
namespace nxrt::services { void register_am(); void am_reset(); }
namespace {
namespace svc = nxrt::services; namespace fs = svc::fs; namespace ipc = nxrt::ipc; namespace k = nxrt::kernel;
using Ptr = std::shared_ptr<ipc::Service>;
constexpr std::array<uint64_t, 2> uid{1, 0x6e78727475736572ull};
struct Answer { uint32_t rc; ipc::Response rsp; };
Answer call(const Ptr& s, uint32_t cmd, const void* data = nullptr, size_t size = 0,
            ipc::Buffer input = {}, ipc::Buffer output = {}) {
    ipc::Request req; req.cmd = cmd; req.in = static_cast<const uint8_t*>(data); req.in_size = size;
    if (input.addr) req.a.push_back(input); if (output.addr) req.b.push_back(output);
    Answer out{}; out.rc = s->handle(req, out.rsp); return out;
}
template<class T> T value(const Answer& a) {
    if (a.rc || a.rsp.out.size() < sizeof(T)) throw std::runtime_error("missing save reply");
    T out; std::memcpy(&out, a.rsp.out.data(), sizeof out); return out;
}
Ptr child(const Answer& a) { return !a.rc && a.rsp.out_objects.size() == 1 ? a.rsp.out_objects[0] : nullptr; }
struct Fixture {
    std::string root; Ptr proxy; uint64_t data = 0;
    std::shared_ptr<std::atomic<bool>> bound = std::make_shared<std::atomic<bool>>(false);
    const nxrt::game::Info* previous = nxrt::game::current();
    Fixture() {
        const char* temp = std::getenv("NXRT_TEST_TEMP");
        root = std::string(temp ? temp : "/tmp") + "/nxrt_save_XXXXXX";
        if (!mkdtemp(root.data())) throw std::runtime_error("create save fixture");
        svc::ps5save::reset(); svc::acc_reset(); k::kernel_reset(); ipc::registry_reset(); nxrt::mem::shutdown();
        nxrt::game::set_current(nullptr); nxrt::config_clear();
        nxrt::config_set("title", "program_id", "0x0123456789abcdef");
        nxrt::config_set("title", "account_save_size", "4096");
        nxrt::config_set("title", "account_save_journal", "4096");
        nxrt::config_set("fs", "save_dir", root.c_str());
        if (!nxrt::mem::init({.heap_reserve = 64ull << 20}) || !(data = nxrt::mem::alloc_kernel(0x4000)))
            throw std::runtime_error("allocate save guest buffers");
        svc::register_acc();
        proxy = std::make_shared<ipc::Service>("save-test"); svc::ps5save::add_commands(*proxy, bound);
    }
    ~Fixture() {
        proxy.reset(); svc::ps5save::reset(); svc::acc_reset(); svc::am_reset(); ipc::registry_reset(); k::kernel_reset(); nxrt::mem::shutdown();
        nxrt::game::set_current(previous); nxrt::config_clear();
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    uint32_t ensure(bool* created = nullptr) { return svc::fsapi::ensure_account_save(uid[0], uid[1], created); }
    std::array<uint8_t, 0x48> attributes() {
        std::array<uint8_t, 0x48> attr{}; attr[0] = 1; attr[40] = 1;
        std::memcpy(attr.data() + 16, uid.data(), 16); return attr;
    }
    Ptr mount(bool ro = false) {
        bound->store(true); const auto attr = attributes(); return child(call(proxy, ro ? 53 : 51, attr.data(), attr.size()));
    }
    ipc::Buffer path(const char* p) { std::strcpy(reinterpret_cast<char*>(data), p); return {data, std::strlen(p) + 1}; }
    Answer create(const Ptr& volume, const char* p, int64_t size = 0) {
        const std::array<int64_t, 2> args{0, size}; return call(volume, 0, args.data(), sizeof args, path(p));
    }
    Ptr open(const Ptr& volume, const char* p, uint32_t mode) { return child(call(volume, 8, &mode, 4, path(p))); }
    Answer write(const Ptr& file, const char* text, int64_t offset = 0) {
        const std::array<int64_t, 3> args{1, offset, static_cast<int64_t>(std::strlen(text))};
        std::strcpy(reinterpret_cast<char*>(data + 0x1000), text);
        return call(file, 1, args.data(), sizeof args, {data + 0x1000, std::strlen(text)});
    }
    std::string read(const Ptr& file) {
        const std::array<int64_t, 3> args{0, 0, 64};
        const auto a = call(file, 0, args.data(), sizeof args, {}, {data + 0x2000, 64});
        return std::string(reinterpret_cast<char*>(data + 0x2000), value<int64_t>(a));
    }
};
}
NXTEST(save_creation_is_idempotent_and_checks_identity_and_binding) {
    Fixture f; int64_t size = 0, journal = 0;
    CHECK_EQ(svc::fsapi::account_save_size(uid[0], uid[1], &size, &journal), fs::rc::kPathNotFound);
    auto attr = f.attributes(); CHECK_EQ(call(f.proxy, 51, attr.data(), attr.size()).rc, k::kInvalidState);
    CHECK_EQ(svc::fsapi::ensure_account_save(9, 9, nullptr), fs::rc::kInvalidArgument);
    bool created = false; REQUIRE(f.ensure(&created) == 0); CHECK(created);
    REQUIRE(f.ensure(&created) == 0); CHECK(!created);
    REQUIRE(svc::fsapi::account_save_size(uid[0], uid[1], &size, &journal) == 0); CHECK_EQ(size, 4096); CHECK_EQ(journal, 4096);
    REQUIRE(f.mount()); attr[40] = 3; CHECK_EQ(call(f.proxy, 51, attr.data(), attr.size()).rc, fs::rc::kInvalidArgument);
    attr = f.attributes(); attr[8] = 1; CHECK_EQ(call(f.proxy, 51, attr.data(), attr.size()).rc, fs::rc::kInvalidArgument);
    CHECK_EQ(call(f.proxy, 51, attr.data(), 16).rc, fs::rc::kInvalidArgument);
}
NXTEST(save_commit_persists_and_uncommitted_writes_are_discarded) {
    Fixture f; REQUIRE(f.ensure() == 0); auto volume = f.mount(); REQUIRE(volume);
    REQUIRE(f.create(volume, "/progress").rc == 0); auto file = f.open(volume, "/progress", 7); REQUIRE(file);
    REQUIRE(f.write(file, "original").rc == 0); CHECK_EQ(call(volume, 10).rc, fs::rc::kTargetLocked);
    file.reset(); REQUIRE(call(volume, 10).rc == 0); volume.reset(); svc::ps5save::reset();
    volume = f.mount(); REQUIRE(volume); file = f.open(volume, "/progress", 7); REQUIRE(file);
    CHECK_EQ(f.read(file), std::string("original")); REQUIRE(f.write(file, "modified").rc == 0);
    file.reset(); volume.reset(); svc::ps5save::reset(); volume = f.mount(); REQUIRE(volume);
    file = f.open(volume, "/progress", 1); REQUIRE(file); CHECK_EQ(f.read(file), std::string("original"));
}
NXTEST(save_read_only_mount_shares_pending_state_without_deleting_it) {
    Fixture f; REQUIRE(f.ensure() == 0); const auto rw = f.mount(); REQUIRE(rw);
    REQUIRE(f.create(rw, "/data").rc == 0); auto file = f.open(rw, "/data", 6); REQUIRE(file);
    REQUIRE(f.write(file, "pending").rc == 0); file.reset();
    const auto ro = f.mount(true); REQUIRE(ro); file = f.open(ro, "/data", 1); REQUIRE(file);
    CHECK_EQ(f.read(file), std::string("pending")); CHECK_EQ(f.create(ro, "/no").rc, fs::rc::kReadOnlyFileSystem);
    CHECK(!f.open(ro, "/data", 2)); file.reset(); REQUIRE(call(rw, 10).rc == 0);
}
NXTEST(save_quota_extension_persists_and_rejects_shrinking) {
    Fixture f; REQUIRE(f.ensure() == 0); auto volume = f.mount(); REQUIRE(volume);
    CHECK(f.create(volume, "/too-big", 4097).rc != 0); REQUIRE(f.create(volume, "/full", 4096).rc == 0);
    auto file = f.open(volume, "/full", 6); REQUIRE(file); CHECK(f.write(file, "x", 4096).rc != 0);
    CHECK_EQ(svc::ps5save::extend(uid[0], uid[1], 2048, 4096), fs::rc::kInvalidArgument);
    REQUIRE(svc::ps5save::extend(uid[0], uid[1], 8192, 8192) == 0); REQUIRE(f.write(file, "x", 4096).rc == 0);
    file.reset(); REQUIRE(call(volume, 10).rc == 0); volume.reset(); svc::ps5save::reset();
    int64_t size = 0, journal = 0; REQUIRE(svc::fsapi::account_save_size(uid[0], uid[1], &size, &journal) == 0);
    CHECK_EQ(size, 8192); CHECK_EQ(journal, 8192);
}
NXTEST(save_paths_buffers_and_file_modes_are_validated) {
    Fixture f; REQUIRE(f.ensure() == 0); const auto volume = f.mount(); REQUIRE(volume);
    CHECK_EQ(f.create(volume, "/../outside").rc, fs::rc::kInvalidArgument);
    REQUIRE(f.create(volume, "/good").rc == 0);
    CHECK(!f.open(volume, "/good", 0)); CHECK(!f.open(volume, "/good", 4)); CHECK(!f.open(volume, "/good", 8));
    const auto file = f.open(volume, "/good", 7); REQUIRE(file);
    std::array<int64_t, 3> args{0, 0, 8};
    CHECK_EQ(call(file, 0, args.data(), sizeof args, {}, {1, 8}).rc, k::kInvalidMemoryState);
    CHECK_EQ(call(file, 1, args.data(), sizeof args, {1, 8}).rc, k::kInvalidMemoryState);
    args[1] = INT64_MAX; CHECK_EQ(call(file, 1, args.data(), sizeof args, {f.data, 8}).rc, fs::rc::kOutOfRange);
    CHECK_EQ(call(file, 1, args.data(), 8, {f.data, 8}).rc, fs::rc::kInvalidArgument);
}
NXTEST(save_directory_and_free_space_replies_match_real_contents) {
    Fixture f; REQUIRE(f.ensure() == 0); const auto volume = f.mount(); REQUIRE(volume);
    REQUIRE(call(volume, 2, nullptr, 0, f.path("/folder")).rc == 0);
    REQUIRE(f.create(volume, "/folder/file", 42).rc == 0);
    CHECK_EQ(value<int64_t>(call(volume, 11, nullptr, 0, f.path("/"))), 4096 - 42);
    CHECK_EQ(value<int64_t>(call(volume, 12, nullptr, 0, f.path("/"))), 4096);
    const uint32_t mode = 3; const auto directory = child(call(volume, 9, &mode, 4, f.path("/folder"))); REQUIRE(directory);
    CHECK_EQ(value<int64_t>(call(directory, 1)), 1);
    CHECK_EQ(value<int64_t>(call(directory, 0, nullptr, 0, {}, {f.data + 0x1000, 0x310})), 1);
    CHECK_EQ(std::string(reinterpret_cast<char*>(f.data + 0x1000)), std::string("file"));
    CHECK_EQ(value<int64_t>(call(directory, 0, nullptr, 0, {}, {f.data + 0x1000, 0x310})), 0);
    CHECK_EQ(call(volume, 3, nullptr, 0, f.path("/folder")).rc, fs::rc::kDirectoryNotEmpty);
    REQUIRE(call(volume, 13, nullptr, 0, f.path("/folder")).rc == 0);
    REQUIRE(call(volume, 3, nullptr, 0, f.path("/folder")).rc == 0); REQUIRE(call(volume, 10).rc == 0);
}
NXTEST(save_expected_first_boot_errors_reach_the_game_without_hiding_failures) {
    using nxrt::diagnostic::stop_on_reply;
    CHECK(!stop_on_reply({true, 0x202, 0}, "nn::fssrv::sf::IFileSystem", 8));
    CHECK(!stop_on_reply({true, 0x402, 0}, "nn::fssrv::sf::IFileSystem", 2));
    CHECK(!stop_on_reply({true, 0x202, 0}, "fsp-srv", 51));
    CHECK(stop_on_reply({true, 0x202, 0}, "fsp-srv", 200));
    CHECK(stop_on_reply({true, fs::rc::kGenericFailure, 0}, "nn::fssrv::sf::IFileSystem", 8));
    CHECK(stop_on_reply({true, fs::rc::kNotImplemented, 0}, "nn::fssrv::sf::IFileSystem", 8));
    CHECK(stop_on_reply({false, 0, 0}, "nn::fssrv::sf::IFileSystem", 8));
}
NXTEST(save_reset_does_not_invalidate_an_open_transaction) {
    Fixture f; REQUIRE(f.ensure() == 0); const auto before = f.mount(); REQUIRE(before);
    REQUIRE(f.create(before, "/pending").rc == 0); auto file = f.open(before, "/pending", 7); REQUIRE(file);
    REQUIRE(f.write(file, "retained").rc == 0); file.reset(); svc::ps5save::reset();
    const auto after = f.mount(); REQUIRE(after); file = f.open(after, "/pending", 1); REQUIRE(file);
    CHECK_EQ(f.read(file), std::string("retained")); file.reset(); REQUIRE(call(before, 10).rc == 0);
}
NXTEST(applet_save_setup_and_size_commands_use_the_persistent_filesystem) {
    Fixture f; svc::am_reset(); svc::register_am(); const auto applet = ipc::make_service("appletOE");
    ipc::Request req; ipc::Response rsp; req.cmd = 10; req.pid = k::current_process()->id;
    req.in_copy.push_back(k::kPseudoCurrentProcess);
    REQUIRE(applet->handle(req, rsp) == 0); REQUIRE(rsp.out_objects.size() == 1);
    const auto app = child(call(rsp.out_objects[0], 20)); REQUIRE(app);
    CHECK_EQ(call(app, 20, uid.data(), 8).rc, k::kInvalidSize);
    CHECK_EQ(value<int64_t>(call(app, 20, uid.data(), sizeof uid)), 0);
    std::array<uint64_t, 5> size{1, uid[0], uid[1], 8192, 8192};
    CHECK_EQ(value<int64_t>(call(app, 26, size.data(), 24)), 4096);
    CHECK_EQ(value<int64_t>(call(app, 25, size.data(), 40)), 0);
    CHECK_EQ(value<int64_t>(call(app, 26, size.data(), 24)), 8192);
    CHECK_EQ(value<int64_t>(call(app, 28)), svc::ps5save::maximum_size);
    REQUIRE(f.mount()); size[0] = 3;
    CHECK_EQ(call(app, 26, size.data(), 24).rc, fs::rc::kNotImplemented);
}
NXTEST(save_missing_data_does_not_silently_replace_existing_metadata) {
    Fixture f; REQUIRE(f.ensure() == 0); svc::ps5save::reset();
    const auto account = f.root + "/0123456789abcdef/00000000000000016e78727475736572";
    REQUIRE(rmdir((account + "/data").c_str()) == 0);
    CHECK_EQ(f.ensure(), fs::rc::kGenericFailure);
    CHECK(std::filesystem::exists(account + "/metadata"));
    CHECK(!std::filesystem::exists(account + "/data"));
}
