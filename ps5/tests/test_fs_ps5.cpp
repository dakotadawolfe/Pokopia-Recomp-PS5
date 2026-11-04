// SPDX-License-Identifier: GPL-3.0-or-later
// Service-level checks use real temporary files and guest memory permissions.
#include "test_framework.h"
#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "../../runtime/src/services/fs_volume.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace nxrt::services { void register_fs(); void fs_reset(); }
NXTEST_GROUP(fs_ps5)

namespace {
namespace ipc = nxrt::ipc;
namespace k = nxrt::kernel;
namespace mem = nxrt::mem;
namespace fs = nxrt::services::fs;
using ServicePtr = std::shared_ptr<ipc::Service>;

struct TempTree {
    std::filesystem::path root;
    TempTree() {
        const char* configured = std::getenv("NXRT_TEST_TEMP");
        const auto base = configured ? std::filesystem::path(configured) : std::filesystem::temp_directory_path();
        std::string name = (base / "nxrt_fsp_XXXXXX").string();
        if (!mkdtemp(name.data())) throw std::system_error(errno, std::generic_category(), "create fsp test directory");
        root = name;
    }
    ~TempTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    void write(const char* value) {
        std::ofstream out(root / "a.bin", std::ios::binary);
        out.write(value, static_cast<std::streamsize>(std::strlen(value)));
        if (!out) throw std::runtime_error("cannot write fsp test asset");
    }
};

struct Fixture {
    TempTree tree;
    const std::string previous_root = nxrt::services::romfs_dir();
    ServicePtr fsp;
    uint64_t data = 0;
    Fixture() {
        nxrt::services::fs_reset(); k::kernel_reset(); ipc::registry_reset(); mem::shutdown();
        if (!mem::init({.heap_reserve = 64ull << 20}) || !(data = mem::alloc_kernel(0x4000)))
            throw std::runtime_error("cannot allocate fsp guest buffer");
        tree.write("ROMFS-backed PS5 data");
        nxrt::config_set("fs", "romfs", tree.root.string().c_str());
        nxrt::services::register_fs();
        fsp = ipc::make_service("fsp-srv");
    }
    ~Fixture() {
        fsp.reset(); nxrt::services::fs_reset(); ipc::registry_reset(); k::kernel_reset(); mem::shutdown();
        nxrt::config_set("fs", "romfs", previous_root.c_str());
    }
    uint32_t bind(const ServicePtr& service) {
        const uint64_t placeholder = 0;
        ipc::Request req; ipc::Response rsp; req.cmd = 1; req.pid = k::current_process()->id;
        req.in = reinterpret_cast<const uint8_t*>(&placeholder); req.in_size = sizeof placeholder;
        return service->handle(req, rsp);
    }
    ServicePtr open(const ServicePtr& service, uint32_t* result = nullptr) {
        ipc::Request req; ipc::Response rsp; req.cmd = 200;
        const auto rc = service->handle(req, rsp);
        if (result) *result = rc;
        return !rc && rsp.out_objects.size() == 1 ? rsp.out_objects[0] : nullptr;
    }
    uint32_t read(const ServicePtr& storage, int64_t at, int64_t size, uint64_t capacity = 0x4000,
                  bool receive_list = false) {
        const std::array<int64_t, 2> arguments{at, size};
        ipc::Request req; ipc::Response rsp; req.cmd = 0;
        req.in = reinterpret_cast<const uint8_t*>(arguments.data()); req.in_size = sizeof arguments;
        (receive_list ? req.c : req.b).push_back({data, capacity});
        return storage->handle(req, rsp);
    }
};

int64_t storage_size(const ServicePtr& storage) {
    ipc::Request req; ipc::Response rsp; req.cmd = 4;
    if (storage->handle(req, rsp) || rsp.out.size() != sizeof(int64_t)) return -1;
    int64_t size; std::memcpy(&size, rsp.out.data(), sizeof size); return size;
}
} // namespace

NXTEST(fsp_requires_actual_current_process_binding) {
    Fixture f; REQUIRE(!f.fsp->is_stub());
    uint32_t result = 0;
    CHECK(!f.open(f.fsp, &result)); CHECK_EQ(result, k::kInvalidState);
    ipc::Request req; ipc::Response rsp; req.cmd = 1;
    CHECK_EQ(f.fsp->handle(req, rsp), fs::rc::kInvalidArgument);
    const uint64_t placeholder = 0;
    req.in = reinterpret_cast<const uint8_t*>(&placeholder); req.in_size = sizeof placeholder;
    CHECK_EQ(f.fsp->handle(req, rsp), k::kInvalidHandle);
    req.pid = k::current_process()->id + 1;
    CHECK_EQ(f.fsp->handle(req, rsp), k::kInvalidHandle);
    CHECK_EQ(f.bind(f.fsp), 0u);
    CHECK(f.open(f.fsp) != nullptr);
    const auto separate = ipc::make_service("fsp-srv");
    CHECK(!f.open(separate, &result)); CHECK_EQ(result, k::kInvalidState);
}

NXTEST(fsp_storage_reads_actual_file_and_header_bytes) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0); const auto storage = f.open(f.fsp); REQUIRE(storage);
    CHECK(storage_size(storage) > 0x200);
    REQUIRE(f.read(storage, 0, 0x50) == 0);
    const auto* header = reinterpret_cast<const uint64_t*>(f.data);
    CHECK_EQ(header[0], 0x50ull); CHECK_EQ(header[9], 0x200ull);
    REQUIRE(f.read(storage, 0x200, 20) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.data), "ROMFS-backed PS5 data", 20) == 0);
    std::memset(reinterpret_cast<void*>(f.data), 0xa5, 20);
    REQUIRE(f.read(storage, 0x200, 20, 20, true) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.data), "ROMFS-backed PS5 data", 20) == 0);
    // Storage ownership is independent of the proxy object.
    f.fsp.reset();
    REQUIRE(f.read(storage, 0x200, 20) == 0);
}

NXTEST(fsp_storage_rejects_malformed_or_out_of_range_reads_without_writes) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0); const auto storage = f.open(f.fsp); REQUIRE(storage);
    const auto size = storage_size(storage); REQUIRE(size > 0);
    std::memset(reinterpret_cast<void*>(f.data), 0xa5, 64);
    for (const auto& pair : {std::array<int64_t, 2>{-1, 1}, {0, -1}, {size, 1}, {size + 1, 0}, {INT64_MAX, 1}, {1, INT64_MAX}}) {
        CHECK_EQ(f.read(storage, pair[0], pair[1]), fs::rc::kOutOfRange);
        CHECK_EQ(*reinterpret_cast<uint64_t*>(f.data), 0xa5a5a5a5a5a5a5a5ull);
    }
    CHECK_EQ(f.read(storage, 0, 16, 15), fs::rc::kInvalidArgument);
    ipc::Request req; ipc::Response rsp; req.cmd = 0;
    CHECK_EQ(storage->handle(req, rsp), fs::rc::kInvalidArgument);
    std::array<int64_t, 2> args{0, 16};
    req.in = reinterpret_cast<const uint8_t*>(args.data()); req.in_size = 8;
    CHECK_EQ(storage->handle(req, rsp), fs::rc::kInvalidArgument);
    req.in_size = sizeof args;
    CHECK_EQ(storage->handle(req, rsp), fs::rc::kInvalidArgument);
    args = {size, 0};
    CHECK_EQ(storage->handle(req, rsp), 0u);
    CHECK_EQ(*reinterpret_cast<uint64_t*>(f.data), 0xa5a5a5a5a5a5a5a5ull);
}

NXTEST(fsp_storage_validates_guest_write_permissions) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0); const auto storage = f.open(f.fsp); REQUIRE(storage);
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::R));
    CHECK_EQ(f.read(storage, 0, 16), k::kInvalidMemoryState);
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::None));
    CHECK_EQ(f.read(storage, 0, 16), k::kInvalidMemoryState);
    REQUIRE(mem::protect(f.data, 0x1000, mem::Prot::RW));
    REQUIRE(mem::protect(f.data + 0x1000, 0x1000, mem::Prot::None));
    const auto saved = f.data; f.data += 0xff8;
    CHECK_EQ(f.read(storage, 0, 16), k::kInvalidMemoryState);
    f.data = UINT64_MAX - 3;
    CHECK_EQ(f.read(storage, 0, 16), k::kInvalidMemoryState);
    f.data = saved;
}

NXTEST(fsp_writes_and_unported_commands_never_report_success) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0); const auto storage = f.open(f.fsp); REQUIRE(storage);
    const auto previous = nxrt::config_get_str("ipc", "stub_result", "");
    nxrt::config_set("ipc", "stub_result", "0");
    ipc::Request req; ipc::Response rsp;
    for (auto command : {1u, 3u}) {
        req.cmd = command; CHECK_EQ(storage->handle(req, rsp), fs::rc::kReadOnlyFileSystem);
    }
    for (auto command : {2u, 5u, 999u}) {
        req.cmd = command; CHECK_EQ(storage->handle(req, rsp), fs::rc::kNotImplemented);
    }
    for (auto command : {2u, 22u, 23u, 51u, 201u, 202u, 203u, 999u}) {
        req.cmd = command; CHECK_EQ(f.fsp->handle(req, rsp), fs::rc::kNotImplemented);
    }
    CHECK(rsp.out.empty() && rsp.out_objects.empty() && rsp.out_move.empty() && rsp.out_copy.empty());
    nxrt::config_set("ipc", "stub_result", previous.c_str());
    REQUIRE(f.read(storage, 0x200, 20) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.data), "ROMFS-backed PS5 data", 20) == 0);
}

NXTEST(fsp_only_current_program_index_can_open_romfs) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0);
    ipc::Request req; ipc::Response rsp; req.cmd = 205;
    CHECK_EQ(f.fsp->handle(req, rsp), fs::rc::kInvalidArgument);
    uint8_t index = 1; req.in = &index; req.in_size = 1;
    CHECK_EQ(f.fsp->handle(req, rsp), fs::rc::kTargetNotFound); CHECK(rsp.out_objects.empty());
    index = 0;
    CHECK_EQ(f.fsp->handle(req, rsp), 0u); REQUIRE(rsp.out_objects.size() == 1);
    CHECK(storage_size(rsp.out_objects[0]) > 0x200);
}

NXTEST(fsp_missing_assets_and_truncated_files_fail_without_placeholder_objects) {
    Fixture f;
    nxrt::services::fs_reset();
    nxrt::config_set("fs", "romfs", (f.tree.root / "missing").string().c_str());
    const auto missing = ipc::make_service("fsp-srv"); REQUIRE(f.bind(missing) == 0);
    uint32_t result = 0; CHECK(!f.open(missing, &result)); CHECK_EQ(result, fs::rc::kTargetNotFound);
    nxrt::services::fs_reset(); nxrt::config_set("fs", "romfs", f.tree.root.string().c_str());
    const auto good = ipc::make_service("fsp-srv"); REQUIRE(f.bind(good) == 0);
    const auto storage = f.open(good); REQUIRE(storage);
    std::error_code error; std::filesystem::resize_file(f.tree.root / "a.bin", 1, error); REQUIRE(!error);
    CHECK_EQ(f.read(storage, 0x200, 20), fs::rc::kGenericFailure);
    CHECK(!f.open(missing, &result)); CHECK_EQ(result, fs::rc::kTargetNotFound);
}

NXTEST(fsp_reset_keeps_open_storage_on_its_original_backing) {
    Fixture f; REQUIRE(f.bind(f.fsp) == 0); const auto old = f.open(f.fsp); REQUIRE(old);
    TempTree next; next.write("New ROMFS tree data!!");
    nxrt::config_set("fs", "romfs", next.root.string().c_str()); nxrt::services::fs_reset();
    const auto proxy = ipc::make_service("fsp-srv"); REQUIRE(f.bind(proxy) == 0);
    const auto fresh = f.open(proxy); REQUIRE(fresh);
    REQUIRE(f.read(fresh, 0x200, 20) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.data), "New ROMFS tree data!!", 20) == 0);
    REQUIRE(f.read(old, 0x200, 20) == 0);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.data), "ROMFS-backed PS5 data", 20) == 0);
}
