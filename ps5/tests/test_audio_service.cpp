// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/audio_sink.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/svc.h"
#include "../../runtime/src/services/audren/Server/BehaviourInfo.h"
#include <array>
#include <cstring>

namespace nxrt::services { void register_audio(); void audio_reset(); }
NXTEST_GROUP(audio_service)
namespace {
namespace ipc = nxrt::ipc;
namespace k = nxrt::kernel;
using Service = std::shared_ptr<ipc::Service>;
struct Reply {
    uint32_t result = 0;
    ipc::Response data;
    template<class T> T get() const { T v{}; if (data.out.size() >= sizeof v) std::memcpy(&v, data.out.data(), sizeof v); return v; }
};
Reply call(const Service& service, uint32_t cmd, std::vector<uint8_t> args = {},
           std::vector<ipc::Buffer> in = {}, std::vector<ipc::Buffer> out = {}) {
    ipc::Request request; request.cmd = cmd; request.in = args.data(); request.in_size = args.size();
    request.a = std::move(in); request.b = std::move(out);
    Reply reply; reply.result = service->handle(request, reply.data); return reply;
}
template<class T> void put(std::vector<uint8_t>& bytes, size_t offset, const T& value) {
    if (bytes.size() < offset + sizeof value) bytes.resize(offset + sizeof value);
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}
nxrt::audren::AudioRendererConfiguration config(int32_t rev = 0x40764552) {
    nxrt::audren::AudioRendererConfiguration c{};
    c.sample_rate = 48000; c.sample_count = 240; c.mix_buffer_count = 4;
    c.sub_mix_buffer_count = 1; c.voice_count = 2; c.sink_count = 1; c.revision = rev;
    return c;
}
std::vector<uint8_t> args(const nxrt::audren::AudioRendererConfiguration& c) {
    std::vector<uint8_t> bytes; put(bytes, 0, c); return bytes;
}
Reply open(const Service& manager, const nxrt::audren::AudioRendererConfiguration& c, uint64_t size) {
    auto bytes = args(c); put(bytes, 0x38, size); put(bytes, 0x40, uint64_t{0x51});
    return call(manager, 0, bytes);
}
struct Fixture {
    uint64_t base = 0;
    Fixture() {
        k::kernel_reset(); ipc::registry_reset(); nxrt::svc_init();
        nxrt::mem::shutdown();
        if (!nxrt::mem::init({.heap_reserve = 64ull << 20}) || !(base = nxrt::mem::alloc_kernel(0x8000))) std::abort();
        nxrt::audio::set_sink(nxrt::audio::make_paced_sink());
        nxrt::services::register_audio();
    }
    ~Fixture() {
        k::kernel_reset(); ipc::registry_reset(); nxrt::services::audio_reset();
        nxrt::audio::set_sink(nullptr); nxrt::mem::shutdown();
    }
};
}

NXTEST(audio_user_proxy_is_reachable_through_guest_ipc) {
    Fixture f; uint32_t handle = 0;
    REQUIRE(ipc::connect_named_port("aud:u", &handle) == 0);
    nxrt::hipc::Message message; message.type = nxrt::hipc::kInvoke2MethodOld;
    put(message.raw, 0, uint32_t{0x49434653}); put(message.raw, 4, uint32_t{0});
    put(message.raw, 8, uint32_t{2}); put(message.raw, 12, uint32_t{0});
    REQUIRE(nxrt::hipc::build(message, reinterpret_cast<uint32_t*>(f.base), 64));
    Ctx ctx{}; ctx.tpidrro = f.base; ctx.x[0] = handle; nxrt::svc_dispatch(&ctx, 0x21);
    REQUIRE(ctx.x[0] == 0);
    nxrt::hipc::Message reply; REQUIRE(nxrt::hipc::parse(reinterpret_cast<uint32_t*>(f.base), 64, reply));
    REQUIRE(reply.move.size() == 1);
    auto session = k::handle_as<ipc::KSession>(reply.move[0]); REQUIRE(session && !session->svc->is_stub());
    CHECK_EQ(call(session->svc, 1, args(config(0x31564552))).result, 0u);
    auto proxy = ipc::make_service("aud:u");
    for (uint32_t command : {0u, 1u, 2u, 3u}) {
        auto result = call(proxy, command); REQUIRE(result.result == 0); REQUIRE(result.data.out_objects.size() == 1);
        CHECK(!result.data.out_objects[0]->is_stub());
        if (command == 3) CHECK_EQ(call(result.data.out_objects[0], 0, std::vector<uint8_t>(4)).result, 0u);
    }
}

NXTEST(audio_native_revision_renderer_signals_and_closes) {
    Fixture f; auto manager = ipc::make_service("audren:u"); const auto cfg = config();
    auto size = call(manager, 1, args(cfg)); REQUIRE(size.result == 0); REQUIRE(size.get<uint64_t>() > 0);
    auto result = open(manager, cfg, size.get<uint64_t>()); REQUIRE(result.result == 0); REQUIRE(result.data.out_objects.size() == 1);
    auto renderer = result.data.out_objects[0];
    CHECK_EQ(call(renderer, 0).get<uint32_t>(), 48000u); CHECK_EQ(call(renderer, 1).get<uint32_t>(), 240u);
    CHECK_EQ(call(renderer, 3).get<uint32_t>(), 1u);
    auto event = call(renderer, 7); REQUIRE(event.data.out_copy.size() == 1);
    const uint32_t h = event.data.out_copy[0]; int32_t index = -1;
    CHECK_EQ(call(renderer, 5).result, 0u);
    CHECK_EQ(k::wait_synchronization(&h, 1, 500000000, &index), 0u);
    CHECK_EQ(call(renderer, 6).result, 0u); CHECK_EQ(call(renderer, 3).get<uint32_t>(), 1u);
    std::memset(reinterpret_cast<void*>(f.base + 0x1000), 0xff, 0x80);
    CHECK(call(renderer, 4, {}, {{f.base + 0x1000, 0x80}}, {{f.base + 0x2000, 0x400}}).result != 0);
    k::handle_close(h);
}

NXTEST(audio_failed_open_preserves_live_renderer_and_session_capacity) {
    Fixture f; auto manager = ipc::make_service("audren:u"); auto cfg = config();
    const auto size = call(manager, 1, args(cfg)).get<uint64_t>(); REQUIRE(size > 0);
    auto first = open(manager, cfg, size); REQUIRE(first.result == 0); REQUIRE(first.data.out_objects.size() == 1);
    CHECK_EQ(open(manager, cfg, 16).result, (4u << 9) | 153u);
    auto bad = cfg; bad.execution_mode = nxrt::audren::server::AudioRendererExecutionMode::Manual;
    CHECK(open(manager, bad, size).result != 0);
    auto second = open(manager, cfg, size); REQUIRE(second.result == 0); REQUIRE(second.data.out_objects.size() == 1);
    CHECK(open(manager, cfg, size).result != 0);
    CHECK_EQ(call(first.data.out_objects[0], 0).get<uint32_t>(), 48000u);
    first.data.out_objects.clear();
    auto replacement = open(manager, cfg, size); REQUIRE(replacement.result == 0);
    CHECK_EQ(call(second.data.out_objects[0], 1).get<uint32_t>(), 240u);
}

NXTEST(audio_revision_validation_and_size_contract) {
    Fixture f; auto manager = ipc::make_service("audren:u"); auto cfg = config(0x31564552); cfg.voice_count = 0;
    CHECK_EQ(call(manager, 1, args(cfg)).get<uint64_t>(), 0x9000u);
    cfg.revision = 0x5a564552; CHECK_EQ(call(manager, 1, args(cfg)).result, (2u << 9) | 153u);
    using B = nxrt::audren::BehaviourInfo;
    CHECK(B::CheckValidRevision(0x40764552)); CHECK(!B::CheckValidRevision(0x41764552));
    CHECK(B::SameRevision(0x40764552, 0x40564552)); CHECK(!B::SameRevision(0x40764552, 0x3f564552));
}
