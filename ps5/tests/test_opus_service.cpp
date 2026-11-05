// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/subpage.h"
#include "nxrt/svc.h"
#include <opus.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nxrt { void svc_memory_reset_for_tests(); }
namespace nxrt::services { void register_opus(); }
NXTEST_GROUP(opus_service)
namespace {
namespace ipc = nxrt::ipc;
namespace k = nxrt::kernel;
namespace m = nxrt::mem;
using Service = std::shared_ptr<ipc::Service>;
template<class T> void put(std::vector<uint8_t>& data, size_t offset, T value) {
    if (data.size() < offset + sizeof value) data.resize(offset + sizeof value);
    std::memcpy(data.data() + offset, &value, sizeof value);
}
struct Reply {
    uint32_t result = 0;
    ipc::Response data;
    uint32_t word(size_t offset = 0) const {
        uint32_t value = 0;
        if (data.out.size() >= offset + 4) std::memcpy(&value, data.out.data() + offset, 4);
        return value;
    }
};
Reply call(const Service& service, uint32_t cmd, std::vector<uint8_t> args = {},
           std::vector<ipc::Buffer> in = {}, std::vector<ipc::Buffer> out = {}, std::vector<uint32_t> copy = {}) {
    ipc::Request req; req.cmd = cmd; req.in = args.data(); req.in_size = args.size();
    req.a = std::move(in); req.b = std::move(out); req.in_copy = std::move(copy);
    Reply reply; reply.result = service->handle(req, reply.data); return reply;
}
std::vector<uint8_t> params(bool extended = false) {
    std::vector<uint8_t> bytes(extended ? 16 : 8);
    put(bytes, 0, int32_t{48000}); put(bytes, 4, int32_t{2}); return bytes;
}
struct Fixture {
    uint64_t base = 0, address = m::kHeapBase + 0x1000;
    Fixture() {
        k::kernel_reset(); ipc::registry_reset(); nxrt::svc_memory_reset_for_tests(); m::shutdown();
        if (!m::init({.heap_reserve = 64ull << 20}) || !(base = m::alloc_kernel(0x20000))) std::abort();
        nxrt::svc_init(); Ctx c{}; c.x[1] = 2 << 20; nxrt::svc_dispatch(&c, 1);
        if (c.x[0]) std::abort();
        nxrt::services::register_opus();
    }
    ~Fixture() { k::kernel_reset(); ipc::registry_reset(); nxrt::svc_memory_reset_for_tests(); m::shutdown(); }
    uint32_t borrow(uint32_t size) {
        Ctx c{}; c.x[1] = address; c.x[2] = size; c.x[3] = 0; nxrt::svc_dispatch(&c, 0x15);
        return c.x[0] ? 0 : c.x[1];
    }
};
std::vector<uint8_t> packet() {
    int error = 0; auto* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    if (!enc || error) std::abort();
    int16_t pcm[1920];
    for (unsigned n = 0; n < 960; ++n) pcm[2*n] = pcm[2*n+1] = int16_t(10000 * std::sin(n * 0.07));
    std::vector<uint8_t> bytes(4096); const int length = opus_encode(enc, pcm, 960, bytes.data()+8, 4088);
    opus_uint32 range = 0; opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&range)); opus_encoder_destroy(enc);
    if (length <= 0) std::abort();
    for (unsigned n = 0; n < 4; ++n) { bytes[n] = uint32_t(length) >> (24-8*n); bytes[4+n] = range >> (24-8*n); }
    bytes.resize(length+8); return bytes;
}
}

NXTEST(opus_sdk_ipc_retains_work_memory_decodes_and_restores_owner) {
    Fixture f; auto manager = ipc::make_service("hwopus");
    auto size_reply = call(manager, 1, params()); REQUIRE(size_reply.result == 0);
    const auto size = size_reply.word(); REQUIRE(size && size % 4096 == 0);
    const auto transfer = f.borrow(size); REQUIRE(transfer);
    uint32_t handle = 0; REQUIRE(ipc::connect_named_port("hwopus", &handle) == 0);
    nxrt::hipc::Message request; request.type = nxrt::hipc::kInvoke2MethodOld;
    put(request.raw, 0, uint32_t{0x49434653}); put(request.raw, 4, uint32_t{0});
    put(request.raw, 8, uint32_t{0}); put(request.raw, 12, uint32_t{0});
    put(request.raw, 16, int32_t{48000}); put(request.raw, 20, int32_t{2}); put(request.raw, 24, size);
    request.copy = {transfer}; REQUIRE(nxrt::hipc::build(request, reinterpret_cast<uint32_t*>(f.base), 64));
    Ctx c{}; c.tpidrro = f.base; c.x[0] = handle; nxrt::svc_dispatch(&c, 0x21); REQUIRE(c.x[0] == 0);
    nxrt::hipc::Message reply; REQUIRE(nxrt::hipc::parse(reinterpret_cast<uint32_t*>(f.base), 64, reply));
    REQUIRE(reply.move.size() == 1); k::handle_close(transfer);
    CHECK(!m::SubpageMap::allows(f.address, size, 1));
    {
        auto session = k::handle_as<ipc::KSession>(reply.move[0]); REQUIRE(session && !session->svc->is_stub());
        auto encoded = packet(); std::memcpy(reinterpret_cast<void*>(f.base+0x1000), encoded.data(), encoded.size());
        auto* output = reinterpret_cast<int16_t*>(f.base+0x4000); std::fill_n(output, 2000, int16_t{1234});
        auto decoded = call(session->svc, 6, {0}, {{f.base+0x1000, encoded.size()}}, {{f.base+0x4000, 4000}});
        REQUIRE(decoded.result == 0); CHECK_EQ(decoded.word(), encoded.size()); CHECK_EQ(decoded.word(4), 960u);
        REQUIRE(decoded.data.out.size() == 16); CHECK_EQ(output[1920], 1234);
        uint64_t energy = 0; for (unsigned n = 0; n < 1920; ++n) energy += std::abs(int(output[n])); CHECK(energy > 100000);
        std::fill_n(output, 2000, int16_t{1234});
        CHECK(call(session->svc, 6, {0}, {{f.base+0x1000, 7}}, {{f.base+0x4000, 4000}}).result != 0);
        CHECK(call(session->svc, 6, {0}, {{f.base+0x1000, encoded.size()}}, {{f.base+0x4000, 10}}).result != 0);
        CHECK(std::all_of(output, output+2000, [](auto value) { return value == 1234; }));
    }
    k::handle_close(reply.move[0]); CHECK(m::SubpageMap::allows(f.address, size, 3)); k::handle_close(handle);
}

NXTEST(opus_extended_parameter_sizes_and_failed_open_keep_transfer_ownership) {
    Fixture f; auto manager = ipc::make_service("hwopus"); auto args = params(true);
    const auto ordinary = call(manager, 5, args); REQUIRE(ordinary.result == 0);
    put(args, 8, uint32_t{1}); auto large = call(manager, 8, args); REQUIRE(large.result == 0); CHECK(large.word() > ordinary.word());
    const auto size = large.word(), transfer = f.borrow(size); REQUIRE(transfer);
    put(args, 16, size-1); CHECK(call(manager, 4, args, {}, {}, {transfer}).result != 0);
    put(args, 16, size); CHECK(call(manager, 4, args, {}, {}, {}).result != 0);
    put(args, 8, uint32_t{2}); CHECK(call(manager, 4, args, {}, {}, {transfer}).result != 0);
    put(args, 8, uint32_t{1}); auto opened = call(manager, 4, args, {}, {}, {transfer}); REQUIRE(opened.result == 0);
    REQUIRE(opened.data.out_objects.size() == 1); k::handle_close(transfer);
    CHECK(!m::SubpageMap::allows(f.address, size, 1)); opened.data.out_objects.clear(); CHECK(m::SubpageMap::allows(f.address, size, 3));
}

NXTEST(opus_multistream_parameter_buffer_layout_and_mapping_validation) {
    Fixture f; auto manager = ipc::make_service("hwopus");
    std::vector<uint8_t> layout(0x118); put(layout, 0, int32_t{48000}); put(layout, 4, int32_t{6});
    put(layout, 8, int32_t{3}); put(layout, 12, int32_t{3}); put(layout, 16, uint32_t{1});
    for (unsigned n = 0; n < 6; ++n) layout[24+n] = n;
    std::memcpy(reinterpret_cast<void*>(f.base+0x1000), layout.data(), layout.size());
    const std::vector<ipc::Buffer> input{{f.base+0x1000, layout.size()}};
    auto size = call(manager, 9, {}, input); REQUIRE(size.result == 0); REQUIRE(size.word());
    const auto transfer = f.borrow(size.word()); REQUIRE(transfer);
    std::vector<uint8_t> args; put(args, 0, size.word());
    *reinterpret_cast<uint8_t*>(f.base+0x1000+24) = 6;
    CHECK(call(manager, 6, args, input, {}, {transfer}).result != 0);
    *reinterpret_cast<uint8_t*>(f.base+0x1000+24) = 0;
    auto opened = call(manager, 6, args, input, {}, {transfer}); REQUIRE(opened.result == 0);
    CHECK_EQ(opened.data.out_objects.size(), size_t{1}); k::handle_close(transfer); opened.data.out_objects.clear();
    CHECK(m::SubpageMap::allows(f.address, size.word(), 3));
}
