// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/svc.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nxrt::services { void register_streamplay(); }
NXTEST_GROUP(streamplay_service)
namespace {
namespace ipc = nxrt::ipc;
namespace k = nxrt::kernel;
namespace m = nxrt::mem;
using Service = std::shared_ptr<ipc::Service>;
void require(bool condition) { if (!condition) throw std::runtime_error("streamplay test fixture validation failed"); }
template<class T> void put(std::vector<uint8_t>& bytes, size_t at, T value) {
    if (bytes.size() < at + sizeof value) bytes.resize(at + sizeof value);
    std::memcpy(bytes.data() + at, &value, sizeof value);
}
struct Reply {
    uint32_t result = 0; ipc::Response data;
    template<class T> T get(size_t at = 0) const {
        T value{}; require(data.out.size() >= at + sizeof value);
        std::memcpy(&value, data.out.data() + at, sizeof value); return value;
    }
};
Reply call(const Service& service, uint32_t cmd, std::vector<uint8_t> args = {}) {
    ipc::Request req; req.cmd = cmd; req.in = args.data(); req.in_size = args.size();
    Reply reply; reply.result = service->handle(req, reply.data); return reply;
}
std::vector<uint8_t> setup(uint8_t type, uint8_t mode, uint32_t guests) {
    std::vector<uint8_t> args(8); put(args, 0, type); put(args, 1, mode); put(args, 4, guests); return args;
}
struct Fixture {
    uint64_t base = 0;
    Fixture() {
        k::kernel_reset(); ipc::registry_reset(); m::shutdown();
        if (!m::init({.heap_reserve = 8ull << 20}) || !(base = m::alloc_kernel(0x1000))) std::abort();
        nxrt::svc_init(); nxrt::services::register_streamplay();
    }
    ~Fixture() { k::kernel_reset(); ipc::registry_reset(); m::shutdown(); }
    Service user() {
        auto root = ipc::make_service("stpl:u"); require(!root->is_stub());
        auto reply = call(root, 0); require(reply.result == 0); require(reply.data.out_objects.size() == 1);
        return reply.data.out_objects[0];
    }
    nxrt::hipc::Message wire(uint32_t handle, uint32_t command) {
        nxrt::hipc::Message message; message.type = nxrt::hipc::kInvoke2MethodOld;
        put(message.raw, 0, uint32_t{0x49434653}); put(message.raw, 4, uint32_t{0});
        put(message.raw, 8, command); put(message.raw, 12, uint32_t{0});
        require(nxrt::hipc::build(message, reinterpret_cast<uint32_t*>(base), 64));
        Ctx ctx{}; ctx.tpidrro = base; ctx.x[0] = handle; nxrt::svc_dispatch(&ctx, 0x21); require(ctx.x[0] == 0);
        nxrt::hipc::Message reply; require(nxrt::hipc::parse(reinterpret_cast<uint32_t*>(base), 64, reply));
        require(reply.raw.size() >= 16); uint32_t result;
        std::memcpy(&result, reply.raw.data() + 8, sizeof result); require(result == 0); return reply;
    }
};
}
NXTEST(streamplay_startup_returns_a_real_user_session_and_hid_copy_handle) {
    Fixture f; uint32_t handle = 0; REQUIRE(ipc::connect_named_port("stpl:u", &handle) == 0);
    auto user = f.wire(handle, 0); REQUIRE(user.move.size() == 1);
    auto hid = f.wire(user.move[0], 300); REQUIRE(hid.copy.size() == 1); REQUIRE(hid.raw.size() >= 32);
    uint64_t size, offset; std::memcpy(&size, hid.raw.data() + 16, 8); std::memcpy(&offset, hid.raw.data() + 24, 8);
    CHECK_EQ(size, uint64_t{0x84000}); CHECK_EQ(offset, uint64_t{0});
    auto shared = k::handle_as<k::KSharedMemory>(hid.copy[0]); REQUIRE(shared);
    CHECK_EQ(shared->size, size); CHECK_EQ(shared->guest_permission, 1u);
    const auto* data = reinterpret_cast<const uint8_t*>(shared->host);
    CHECK(std::all_of(data, data + size, [](uint8_t value) { return value == 0; }));
    k::handle_close(handle); k::handle_close(user.move[0]); k::handle_close(hid.copy[0]);
}
NXTEST(streamplay_hid_reuses_backing_and_survives_duplicate_handle_closes) {
    Fixture f; auto user = f.user(); auto a = call(user, 300), b = call(f.user(), 300);
    REQUIRE(a.result == 0 && b.result == 0); REQUIRE(a.data.out_copy.size() == 1 && b.data.out_copy.size() == 1);
    const auto first = a.data.out_copy[0], second = b.data.out_copy[0]; CHECK(first != second);
    REQUIRE(k::handle_as<k::KSharedMemory>(first) == k::handle_as<k::KSharedMemory>(second));
    constexpr uint64_t address = 0x6000000000ull, size = 0x84000;
    CHECK_EQ(k::shared_memory_map(first, address, size, 3), k::kInvalidPermission);
    REQUIRE(k::shared_memory_map(first, address, size, 1) == 0);
    k::handle_close(first); user.reset();
    const auto* data = reinterpret_cast<const uint8_t*>(address);
    CHECK(std::all_of(data, data + size, [](uint8_t value) { return value == 0; }));
    REQUIRE(k::shared_memory_unmap(second, address, size) == 0); k::handle_close(second);
}
NXTEST(streamplay_work_sizes_validate_counts_and_match_offline_buffer_parts) {
    Fixture f; auto user = f.user();
    auto parts = call(user, 9, {0, 0}); REQUIRE(parts.result == 0); REQUIRE(parts.data.out.size() == 32);
    for (unsigned n = 0; n < 4; ++n) CHECK_EQ(parts.get<uint64_t>(n * 8), uint64_t{0x1000});
    for (unsigned guests = 0; guests <= 3; ++guests) {
        auto ordinary = call(user, 1, setup(0, 0, guests)), remote = call(user, 1, setup(1, 1, guests));
        REQUIRE(ordinary.result == 0 && remote.result == 0);
        CHECK_EQ(ordinary.get<uint32_t>(), 0x1000u + guests * 0x3000u);
        CHECK_EQ(remote.get<uint32_t>(), guests * 0x1000u);
    }
    CHECK(call(user, 1, setup(0, 0, 4)).result != 0); CHECK(call(user, 1, setup(2, 0, 1)).result != 0);
    CHECK(call(user, 1, std::vector<uint8_t>(7)).result != 0); CHECK(call(user, 9, {0}).result != 0);
}
NXTEST(streamplay_offline_events_state_and_finalize_preserve_disconnection) {
    Fixture f; auto user = f.user(); auto other = f.user();
    auto event = call(user, 7), event2 = call(other, 7); REQUIRE(event.result == 0 && event2.result == 0);
    REQUIRE(event.data.out_copy.size() == 1 && event2.data.out_copy.size() == 1);
    auto first = k::handle_as<k::KEvent>(event.data.out_copy[0]), second = k::handle_as<k::KEvent>(event2.data.out_copy[0]);
    REQUIRE(first && second); CHECK(first->state == second->state); CHECK(!first->writable); CHECK(!first->signaled());
    int32_t index = -1; const auto handle = event.data.out_copy[0];
    CHECK_EQ(k::wait_synchronization(&handle, 1, 0, &index), k::kTimedOut);
    CHECK_EQ(call(user, 2, setup(1, 1, 3)).result, 0u); CHECK(call(other, 2, setup(1, 1, 3)).result != 0);
    CHECK_EQ(call(other, 502).get<uint8_t>(), uint8_t{1}); CHECK_EQ(call(other, 503).get<uint8_t>(), uint8_t{1});
    CHECK_EQ(call(user, 8).get<uint64_t>(), uint64_t{0}); CHECK_EQ(call(user, 5).result, 0u);
    CHECK_EQ(call(user, 3).result, 0u); CHECK_EQ(call(other, 502).get<uint8_t>(), uint8_t{0});
    CHECK_EQ(call(other, 2, setup(0, 0, 1)).result, 0u); CHECK(!second->signaled());
    std::vector<uint8_t> volume(16); put(volume, 0, std::numeric_limits<float>::quiet_NaN());
    CHECK(call(user, 510, volume).result != 0); put(volume, 0, 1.0f); CHECK_EQ(call(user, 510, volume).result, 0u);
    CHECK(call(user, 25, {}).result != 0); CHECK_EQ(call(user, 25, std::vector<uint8_t>(4)).result, 0u);
    CHECK_EQ(call(user, 27, std::vector<uint8_t>(4)).result, 0u);
    k::handle_close(event.data.out_copy[0]); k::handle_close(event2.data.out_copy[0]);
}
NXTEST(streamplay_handle_exhaustion_and_unimplemented_streaming_never_report_success) {
    Fixture f; auto user = f.user(); auto initial = call(user, 300); REQUIRE(initial.result == 0);
    k::handle_close(initial.data.out_copy[0]);
    auto object = std::make_shared<k::KObject>();
    for (unsigned n = 0; n < 0x3000; ++n) REQUIRE(k::handle_add(object));
    auto memory = call(user, 300), event = call(user, 7);
    CHECK_EQ(memory.result, k::kOutOfHandles); CHECK_EQ(event.result, k::kOutOfHandles);
    CHECK(memory.data.out_copy.empty()); CHECK(event.data.out_copy.empty()); k::handles_reset();
    auto retry = call(user, 300); REQUIRE(retry.result == 0); k::handle_close(retry.data.out_copy[0]);
    for (uint32_t cmd : {4u, 500u, 501u, 504u, 505u, 506u, 507u, 509u, 999u}) {
        auto reply = call(user, cmd); CHECK_EQ(reply.result, k::kStubService);
        CHECK(reply.data.out.empty()); CHECK(reply.data.out_objects.empty()); CHECK(reply.data.out_copy.empty());
    }
}
