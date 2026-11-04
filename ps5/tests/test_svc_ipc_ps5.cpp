// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/svc.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "nxrt/am_ps5.h"
#include "nxrt/acc_ps5.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/services.h"
#include "trace_ipc_reply.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nxrt::services { void register_logfault(); void logfault_reset(); uint32_t logfault_destination(); }
namespace nxrt::services { void register_am(); void am_reset(); }
namespace nxrt::services { void register_aoc(); void aoc_reset(); }
namespace nxrt::services { void register_pctl(); }
namespace nxrt::services { void register_fs(); void fs_reset(); }

NXTEST_GROUP(svc_ipc_ps5)
namespace {
namespace k = nxrt::kernel;
namespace mem = nxrt::mem;
namespace ipc = nxrt::ipc;
namespace hipc = nxrt::hipc;
uint32_t call(Ctx& c, unsigned n) { nxrt::svc_dispatch(&c, n); return c.x[0]; }
template<class T> void push(std::vector<uint8_t>& bytes, T v) {
    const auto offset = bytes.size(); bytes.resize(offset + sizeof v); std::memcpy(bytes.data() + offset, &v, sizeof v);
}
uint32_t word(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t v = UINT32_MAX; if (offset + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + offset, 4); return v;
}
hipc::Message request(uint32_t command, bool tipc = false) {
    hipc::Message m; m.type = tipc ? hipc::kTipcBase + command : hipc::kInvoke2MethodOld;
    if (!tipc) { push(m.raw, 0x49434653u); push(m.raw, 0u); push(m.raw, command); push(m.raw, 0u); }
    return m;
}
struct Fixture {
    uint64_t base = 0; unsigned calls = 0; Ctx c{};
    Fixture() {
        k::kernel_reset(); ipc::registry_reset(); mem::shutdown(); nxrt::svc_init();
        if (!mem::init({.heap_reserve = 64ull << 20}) || !(base = mem::alloc_kernel(0x4000))) std::abort();
        c.tpidrro = base;
        ipc::register_service("test", [this] {
            auto s = std::make_shared<ipc::Service>("test");
            s->add_cmd(7, "Echo", [this](ipc::Request& req, ipc::Response& rsp) { ++calls; rsp.push(req.arg<uint32_t>(0)); return 0; });
            s->add_cmd(8, "Throw", [](ipc::Request&, ipc::Response&) -> uint32_t { throw std::runtime_error("fixture"); });
            return s;
        });
    }
    ~Fixture() { k::kernel_reset(); ipc::registry_reset(); mem::shutdown(); }
    uint32_t connect(const char* name) {
        std::strcpy(reinterpret_cast<char*>(base + 0x3000), name); c.x[1] = base + 0x3000;
        if (call(c, 0x1f)) return 0; return c.x[1];
    }
    void write(const hipc::Message& m, uint64_t address = 0) {
        if (!hipc::build(m, reinterpret_cast<uint32_t*>(address ? address : base), 0x100 / 4)) std::abort();
    }
    uint32_t send(uint32_t h, const hipc::Message& m) { write(m); c.x[0] = h; return call(c, 0x21); }
    hipc::Message reply(bool tipc = false) {
        std::array<uint32_t, 64> words{}; std::memcpy(words.data(), reinterpret_cast<void*>(base), sizeof words);
        if (tipc) words[0] = (words[0] & 0xffff0000) | hipc::kTipcBase;
        hipc::Message m; if (!hipc::parse(words.data(), words.size(), m)) std::abort(); return m;
    }
};
}
NXTEST(ipc_connect_and_close_preserve_session_ownership) {
    Fixture f; const auto count = k::handle_count(); const auto h = f.connect("sm:"); REQUIRE(h);
    CHECK_EQ(k::handle_count(), count + 1); CHECK_EQ(k::handle_as<ipc::KSession>(h)->svc->name(), std::string("sm:"));
    f.c.x[0] = h; CHECK_EQ(call(f.c, 0x16), k::kOk); CHECK(!k::handle_get(h));
}
NXTEST(ipc_port_names_are_bounded_and_respect_guest_page_permissions) {
    Fixture f; const auto name = f.base + 0xffc; std::memcpy(reinterpret_cast<void*>(name), "sm:", 4);
    REQUIRE(mem::protect(f.base + 0x1000, 0x1000, mem::Prot::None));
    f.c.x[1] = name; CHECK_EQ(call(f.c, 0x1f), k::kOk); k::handle_close(f.c.x[1]);
    *reinterpret_cast<char*>(name + 3) = 'x'; f.c.x[1] = name;
    CHECK_EQ(call(f.c, 0x1f), k::kInvalidPointer); CHECK_EQ(f.c.x[1], 0u);
    std::memset(reinterpret_cast<void*>(f.base), 'x', 12); f.c.x[1] = f.base;
    CHECK_EQ(call(f.c, 0x1f), k::kOutOfRange); CHECK_EQ(f.connect("absent"), 0u); CHECK_EQ(f.c.x[0], k::kNotFound);
    f.c.x[1] = UINT64_MAX; CHECK_EQ(call(f.c, 0x1f), k::kInvalidPointer);
}
NXTEST(ipc_sm_registration_and_service_lookup_work_in_cmif_and_tipc) {
    Fixture f;
    for (bool tipc : {false, true}) {
        const auto sm = f.connect("sm:"); REQUIRE(sm);
        auto m = request(0, tipc); m.has_pid = true; m.pid = 91; push(m.raw, uint64_t{0});
        CHECK_EQ(f.send(sm, m), k::kOk); CHECK_EQ(word(f.reply(tipc).raw, tipc ? 0 : 8), 0u);
        m = request(1, tipc); const char name[8] = "test"; m.raw.insert(m.raw.end(), name, name + 8);
        CHECK_EQ(f.send(sm, m), k::kOk); auto r = f.reply(tipc); REQUIRE(r.move.size() == 1);
        const auto h = r.move[0]; auto echo = request(7, tipc); push(echo.raw, 0x91827364u);
        CHECK_EQ(f.send(h, echo), k::kOk); CHECK_EQ(word(f.reply(tipc).raw, tipc ? 4 : 16), 0x91827364u);
        k::handle_close(h); k::handle_close(sm);
    }
    CHECK_EQ(f.calls, 2u);
}
NXTEST(ipc_tls_buffer_must_be_fully_readable_and_writable) {
    Fixture f; const auto h = f.connect("test"); auto m = request(7); push(m.raw, 77u); f.write(m);
    REQUIRE(mem::protect(f.base, 0x1000, mem::Prot::R)); f.c.x[0] = h;
    CHECK_EQ(call(f.c, 0x21), k::kInvalidMemoryState); CHECK_EQ(f.calls, 0u);
    REQUIRE(mem::protect(f.base, 0x1000, mem::Prot::RW));
    f.c.tpidrro = f.base + 0x3f80; f.c.x[0] = h; CHECK_EQ(call(f.c, 0x21), k::kInvalidMemoryState);
}
NXTEST(ipc_user_buffer_checks_guest_alignment_size_overflow_and_contents) {
    Fixture f; const auto h = f.connect("test"); auto m = request(7); push(m.raw, 123u); f.write(m);
    auto send = [&](uint64_t p, uint64_t size) { f.c.x[0] = p; f.c.x[1] = size; f.c.x[2] = h; return call(f.c, 0x22); };
    CHECK_EQ(send(f.base + 1, 0x1000), k::kInvalidAddress); CHECK_EQ(send(f.base, 0), k::kInvalidSize);
    CHECK_EQ(send(f.base, 0x100), k::kInvalidSize); CHECK_EQ(send(UINT64_MAX - 0xfff, 0x1000), k::kInvalidMemoryState);
    CHECK_EQ(send(f.base, 0x2000), k::kNotImplemented); CHECK_EQ(send(f.base, 0x1000), k::kOk);
    CHECK_EQ(word(f.reply().raw, 16), 123u); CHECK_EQ(f.calls, 1u);
}
NXTEST(ipc_descriptors_require_permissions_before_calling_a_service) {
    Fixture f; const auto h = f.connect("test"); const auto data = f.base + 0x1000;
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::R));
    for (unsigned type = 0; type < 5; ++type) {
        auto m = request(7); push(m.raw, 42u);
        switch (type) {
            case 0: m.send.push_back({data, 4, 0}); break;
            case 1: m.ptrs.push_back({data, 4, 0}); break;
            case 2: m.recv.push_back({data, 4, 0}); break;
            case 3: m.exch.push_back({data, 4, 0}); break;
            case 4: m.recv_list_flags = 2; m.recv_list.push_back({data, 4}); break;
        }
        const auto old = f.calls; CHECK_EQ(f.send(h, m), type < 2 ? k::kOk : k::kInvalidMemoryState);
        CHECK_EQ(f.calls, old + (type < 2 ? 1u : 0u));
    }
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::None));
    auto m = request(7); m.send.push_back({data, 8, 0}); CHECK_EQ(f.send(h, m), k::kInvalidMemoryState);
    m.send[0] = {f.base + 0xffc, 8, 0}; CHECK_EQ(f.send(h, m), k::kInvalidMemoryState);
}
NXTEST(ipc_malformed_messages_and_invalid_handles_do_not_dispatch) {
    Fixture f; const auto h = f.connect("test"); auto m = request(7);
    CHECK_EQ(f.send(0x123, m), k::kInvalidHandle);
    const auto event = k::handle_add(std::make_shared<k::KEvent>()); CHECK_EQ(f.send(event, m), k::kInvalidHandle);
    m.copy = {0x123}; CHECK_EQ(f.send(h, m), k::kInvalidHandle);
    m.copy.clear(); m.move = {0x123}; CHECK_EQ(f.send(h, m), k::kInvalidHandle);
    std::memset(reinterpret_cast<void*>(f.base), 0xff, 0x100); f.c.x[0] = h;
    CHECK_EQ(call(f.c, 0x21), k::kInvalidCombination); CHECK_EQ(f.calls, 0u);
}
NXTEST(ipc_service_errors_are_in_the_reply_and_release_keeps_the_handle) {
    Fixture f; const auto h = f.connect("test"); CHECK_EQ(f.send(h, request(8)), k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::kInvalidState);
    CHECK_EQ(f.send(h, request(99)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kStubService);
    hipc::Message close; close.type = hipc::kRelease; CHECK_EQ(f.send(h, close), k::kOk); CHECK(k::handle_get(h) != nullptr);
}
NXTEST(ipc_control_clone_keeps_the_service_alive_after_original_close) {
    Fixture f; const auto h = f.connect("test"); auto clone = request(2); clone.type = hipc::kInvoke2ManagerMethodOld;
    CHECK_EQ(f.send(h, clone), k::kOk); auto reply = f.reply(); REQUIRE(reply.move.size() == 1);
    const auto copy = reply.move[0]; k::handle_close(h); auto m = request(7); push(m.raw, 456u);
    CHECK_EQ(f.send(copy, m), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 456u);
}
NXTEST(ipc_domain_conversion_and_object_request_reach_the_service) {
    Fixture f; const auto h = f.connect("test"); auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    CHECK_EQ(f.send(h, convert), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 1u);
    auto inner = request(7); push(inner.raw, 789u); hipc::Message m; m.type = hipc::kInvoke2MethodOld;
    push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, static_cast<uint16_t>(inner.raw.size()));
    push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0}); m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
    CHECK_EQ(f.send(h, m), k::kOk); CHECK_EQ(word(f.reply().raw, 32), 789u); CHECK_EQ(f.calls, 1u);
}

namespace {
struct LogCapture {
    nxrt::LogLevel old = nxrt::log_level(); std::vector<std::string> lines;
    LogCapture() {
        nxrt::log_set_level(nxrt::LogLevel::Trace);
        nxrt::log_set_sink(+[](nxrt::LogLevel, const char* line, void* self) {
            if (std::strstr(line, "guestlog:")) static_cast<LogCapture*>(self)->lines.emplace_back(line);
        }, this);
    }
    ~LogCapture() { nxrt::log_set_sink(nullptr, nullptr); nxrt::log_set_level(old); }
};
uint32_t open_logger(Fixture& f) {
    nxrt::services::logfault_reset(); nxrt::services::register_logfault(); const auto lm = f.connect("lm");
    if (!lm || f.send(lm, request(0))) return 0;
    const auto reply = f.reply(); return reply.move.size() == 1 ? reply.move[0] : 0;
}
uint32_t log_packet(Fixture& f, uint32_t logger, unsigned flags, const char* text) {
    std::vector<uint8_t> p(0x18); p[0x10] = flags; p[0x12] = 1;
    const uint32_t payload = 2 + std::strlen(text); std::memcpy(p.data() + 0x14, &payload, 4);
    p.push_back(2); p.push_back(static_cast<uint8_t>(std::strlen(text))); p.insert(p.end(), text, text + std::strlen(text));
    std::memcpy(reinterpret_cast<void*>(f.base + 0x1000), p.data(), p.size());
    auto m = request(0); m.send.push_back({f.base + 0x1000, p.size(), 0}); return f.send(logger, m);
}
}
NXTEST(ipc_log_manager_opens_real_logger_and_stores_destination) {
    Fixture f; const auto logger = open_logger(f); REQUIRE(logger);
    CHECK(!k::handle_as<ipc::KSession>(logger)->svc->is_stub());
    auto m = request(1); push(m.raw, 4u); CHECK_EQ(f.send(logger, m), k::kOk);
    CHECK_EQ(nxrt::services::logfault_destination(), 4u);
    nxrt::services::logfault_reset(); CHECK_EQ(nxrt::services::logfault_destination(), 1u);
    CHECK(!ipc::has_service("fatal:u")); CHECK(!ipc::has_service("fatal:p"));
}
NXTEST(ipc_log_packets_assemble_across_calls_and_escape_newlines) {
    Fixture f; const auto logger = open_logger(f); REQUIRE(logger); LogCapture capture;
    CHECK_EQ(log_packet(f, logger, 1, "hello "), k::kOk); CHECK(capture.lines.empty());
    CHECK_EQ(log_packet(f, logger, 2, "PS5\n"), k::kOk); REQUIRE(capture.lines.size() == 1);
    CHECK(capture.lines[0].find("hello PS5\\n") != std::string::npos);
}
NXTEST(ipc_log_rejects_unreadable_buffers_and_drops_short_packets) {
    Fixture f; const auto logger = open_logger(f); REQUIRE(logger); LogCapture capture;
    auto m = request(0); m.send.push_back({f.base + 0x1000, 8, 0}); CHECK_EQ(f.send(logger, m), k::kOk);
    REQUIRE(capture.lines.size() == 1); CHECK(capture.lines[0].find("shorter") != std::string::npos);
    REQUIRE(mem::protect(f.base + 0x1000, 0x1000, mem::Prot::None));
    CHECK_EQ(f.send(logger, m), k::kInvalidMemoryState); CHECK_EQ(capture.lines.size(), size_t{1});
}
NXTEST(error_context_endpoint_cannot_claim_to_persist_an_unbacked_report) {
    Fixture f; nxrt::services::register_logfault();
    auto writer = ipc::make_service("ectx:aw"); REQUIRE(writer); CHECK(!writer->is_stub());
    ipc::Request req; ipc::Response rsp; req.cmd = 0;
    CHECK_EQ(writer->handle(req, rsp), 0u); REQUIRE(rsp.out_objects.size() == 1);
    auto registrar = rsp.out_objects[0]; writer.reset();
    ipc::Response completion; CHECK_EQ(registrar->handle(req, completion), k::kNotImplemented);
    CHECK(completion.out.empty() && completion.out_copy.empty() && completion.out_move.empty());
    writer = ipc::make_service("ectx:aw"); req.cmd = 1; ipc::Response commit;
    CHECK_EQ(writer->handle(req, commit), k::kNotImplemented);
    CHECK(commit.out.empty());
}

NXTEST(ipc_copies_resolve_process_identity_and_preserve_sender_ownership) {
    Fixture f; uint32_t received = 0; uint64_t pid = 0;
    std::shared_ptr<k::KProcess> retained;
    ipc::register_service("copy", [&] {
        auto s = std::make_shared<ipc::Service>("copy");
        s->add_cmd(0, "RetainProcess", [&](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (req.in_copy.size() != 1) return k::kInvalidHandle;
            received = req.in_copy[0]; pid = req.pid;
            retained = k::handle_as<k::KProcess>(received); return retained ? 0 : k::kInvalidHandle;
        }); return s;
    });
    const auto h = f.connect("copy"); const auto original = k::handle_add(k::current_process());
    for (const auto source : {original, k::kPseudoCurrentProcess}) {
        auto m = request(0); m.copy = {source}; m.has_pid = true; m.pid = UINT64_MAX;
        const auto count = k::handle_count(); CHECK_EQ(f.send(h, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
        CHECK(received != source); CHECK(!k::handle_get(received)); CHECK_EQ(k::handle_count(), count);
        CHECK(retained == k::current_process()); CHECK_EQ(pid, retained->id); CHECK(k::handle_get(source) != nullptr);
    }
    k::handle_close(original); CHECK(retained == k::current_process());
}
NXTEST(ipc_pseudo_moves_fail_and_copy_allocation_rolls_back) {
    Fixture f; const auto h = f.connect("test");
    for (auto pseudo : {k::kPseudoCurrentProcess, k::kPseudoCurrentThread}) {
        auto m = request(7); m.move = {pseudo}; CHECK_EQ(f.send(h, m), k::kInvalidHandle);
    }
    auto object = std::make_shared<k::KEvent>(); std::vector<uint32_t> handles;
    for (;;) { const auto handle = k::handle_add(object); if (!handle) break; handles.push_back(handle); }
    REQUIRE(!handles.empty()); k::handle_close(handles.back()); handles.pop_back();
    const auto count = k::handle_count(); auto m = request(7); m.copy = {k::kPseudoCurrentProcess, k::kPseudoCurrentProcess};
    CHECK_EQ(f.send(h, m), k::kOutOfHandles); CHECK_EQ(k::handle_count(), count); CHECK_EQ(f.calls, 0u);
    for (auto handle : handles) k::handle_close(handle);
}
namespace {
struct AmFixture : Fixture {
    uint32_t am = 0;
    AmFixture() { nxrt::services::am_reset(); nxrt::services::register_am(); am = connect("appletOE"); }
    ~AmFixture() { nxrt::services::am_reset(); }
    uint32_t open(bool tipc = false) {
        auto m = request(10, tipc); push(m.raw, uint64_t{0}); m.copy = {k::kPseudoCurrentProcess}; m.has_pid = true;
        if (send(am, m)) return 0;
        auto r = reply(tipc); return word(r.raw, tipc ? 0 : 8) == 0 && r.move.size() == 1 ? r.move[0] : 0;
    }
    uint32_t child(uint32_t parent, unsigned command) {
        if (send(parent, request(command))) return 0;
        auto r = reply(); return word(r.raw, 8) == 0 && r.move.size() == 1 ? r.move[0] : 0;
    }
    uint32_t receive(uint32_t common) {
        if (send(common, request(1))) return UINT32_MAX;
        auto r = reply(); return word(r.raw, 8) == 0 ? word(r.raw, 16) : UINT32_MAX;
    }
    uint32_t storage(uint32_t creator, int64_t size) {
        auto m = request(10); push(m.raw, size);
        if (send(creator, m)) return 0;
        auto r = reply(); return word(r.raw, 8) == 0 && r.move.size() == 1 ? r.move[0] : 0;
    }
};
hipc::Message remote_boot(uint64_t aruid) {
    auto m = request(4, true); m.copy = {k::kPseudoCurrentProcess};
    for (uint64_t value : {aruid, uint64_t{0x8000000000000005ull}, uint64_t{0}, uint64_t{0}, uint64_t{0}, uint64_t{0}})
        push(m.raw, value);
    return m;
}
uint32_t tipc_result(AmFixture& f, uint32_t handle, const hipc::Message& request) {
    if (f.send(handle, request)) return UINT32_MAX;
    const auto reply = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(f.base), 64, true);
    return reply.valid ? reply.result : UINT32_MAX;
}
}
NXTEST(graphics_supervisor_returns_an_independent_tipc_session_from_a_domain) {
    AmFixture f; const auto self = f.child(f.open(), 1); REQUIRE(self);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(self, convert) == k::kOk);
    auto inner = request(2000); auto m = request(0); m.raw.clear();
    push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, uint16_t{16});
    push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0});
    m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
    REQUIRE(f.send(self, m) == k::kOk);
    const auto reply = f.reply(); CHECK_EQ(word(reply.raw, 0), 0u); CHECK_EQ(word(reply.raw, 24), 0u);
    REQUIRE(reply.move.size() == 1); CHECK(reply.copy.empty());
    const auto gsv = k::handle_as<ipc::KSession>(reply.move[0]); REQUIRE(gsv);
    CHECK(!gsv->domain); CHECK_EQ(gsv->svc->name(), std::string("GraphicsSupervisor"));
    k::handle_close(self);
    CHECK_EQ(tipc_result(f, reply.move[0], remote_boot(k::current_process()->id)), k::kOk);
}
NXTEST(graphics_remote_boot_retains_process_ownership_and_context_state) {
    AmFixture f; const auto self = f.child(f.open(), 1); const auto gsv = f.child(self, 2000); REQUIRE(gsv);
    const auto count = k::handle_count(); const auto boot = remote_boot(k::current_process()->id);
    CHECK_EQ(tipc_result(f, gsv, boot), k::kOk); CHECK_EQ(k::handle_count(), count);
    auto bind = request(11, true); bind.copy = {k::kPseudoCurrentProcess};
    CHECK_EQ(tipc_result(f, gsv, bind), k::kOk); CHECK_EQ(k::handle_count(), count);
    CHECK_EQ(tipc_result(f, gsv, request(101, true)), k::kOk);
    CHECK_EQ(tipc_result(f, gsv, boot), k::kInvalidState);
    CHECK_EQ(tipc_result(f, gsv, bind), k::kInvalidState);
    CHECK_EQ(tipc_result(f, gsv, request(101, true)), k::kInvalidState);
}
NXTEST(graphics_boot_rejects_unsupported_versions_usage_and_reserved_fields) {
    AmFixture f; const auto gsv = f.child(f.child(f.open(), 1), 2000); REQUIRE(gsv);
    const auto boot = remote_boot(k::current_process()->id);
    for (const auto offset : {8u, 16u, 24u, 32u, 40u}) {
        auto m = boot; const uint64_t bad = 9; std::memcpy(m.raw.data() + offset, &bad, 8);
        CHECK_EQ(tipc_result(f, gsv, m), offset == 8 || offset == 40 ? k::kNotImplemented : k::kInvalidCombination);
        const auto reply = f.reply(true); CHECK(reply.copy.empty() && reply.move.empty()); CHECK_EQ(reply.raw.size(), size_t{4});
    }
    auto wrong_id = remote_boot(k::current_process()->id + 1);
    CHECK_EQ(tipc_result(f, gsv, wrong_id), k::kInvalidCombination);
    CHECK_EQ(tipc_result(f, gsv, boot), k::kOk); // Rejections did not partially initialize the session.
}
NXTEST(graphics_boot_requires_one_copy_of_the_owning_process_and_exact_layout) {
    AmFixture f; const auto gsv = f.child(f.child(f.open(), 1), 2000); REQUIRE(gsv);
    const auto boot = remote_boot(k::current_process()->id);
    for (size_t size : {0u, 40u, 44u, 52u}) {
        auto m = boot; m.raw.resize(size); CHECK_EQ(tipc_result(f, gsv, m), k::kInvalidSize);
    }
    auto m = boot; m.copy.clear(); CHECK_EQ(tipc_result(f, gsv, m), k::kInvalidHandle);
    m = boot; m.copy.push_back(k::kPseudoCurrentProcess); CHECK_EQ(tipc_result(f, gsv, m), k::kInvalidHandle);
    const auto other = k::handle_add(std::make_shared<k::KProcess>());
    m = boot; m.copy = {other}; CHECK_EQ(tipc_result(f, gsv, m), k::kInvalidHandle); k::handle_close(other);
    m = boot; m.send.push_back({f.base + 0x1000, 8, 0}); CHECK_EQ(tipc_result(f, gsv, m), k::kInvalidCombination);
    CHECK_EQ(tipc_result(f, gsv, boot), k::kOk);
}
NXTEST(graphics_context_requires_boot_and_binding_and_refuses_cmif) {
    AmFixture f; const auto gsv = f.child(f.child(f.open(), 1), 2000); REQUIRE(gsv);
    auto bind = request(11, true); bind.copy = {k::kPseudoCurrentProcess};
    CHECK_EQ(tipc_result(f, gsv, bind), k::kInvalidState);
    CHECK_EQ(tipc_result(f, gsv, request(101, true)), k::kInvalidState);
    REQUIRE(f.send(gsv, request(101)) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidCombination);
    CHECK_EQ(tipc_result(f, gsv, remote_boot(k::current_process()->id)), k::kOk);
    CHECK_EQ(tipc_result(f, gsv, request(101, true)), k::kInvalidState);
    auto malformed = bind; push(malformed.raw, 1u); CHECK_EQ(tipc_result(f, gsv, malformed), k::kInvalidSize);
    CHECK_EQ(tipc_result(f, gsv, bind), k::kOk);
    malformed = request(101, true); malformed.copy = {k::kPseudoCurrentProcess};
    CHECK_EQ(tipc_result(f, gsv, malformed), k::kInvalidHandle);
    CHECK_EQ(tipc_result(f, gsv, request(101, true)), k::kOk);
}
NXTEST(graphics_sessions_share_application_state_and_reset_keeps_old_handles_valid) {
    AmFixture f; const auto self = f.child(f.open(), 1);
    const auto first = f.child(self, 2000), second = f.child(self, 2000); REQUIRE(first && second);
    CHECK_EQ(tipc_result(f, first, remote_boot(k::current_process()->id)), k::kOk);
    k::handle_close(first); auto bind = request(11, true); bind.copy = {k::kPseudoCurrentProcess};
    CHECK_EQ(tipc_result(f, second, bind), k::kOk);
    nxrt::services::am_reset();
    const auto fresh = f.child(f.child(f.open(), 1), 2000); REQUIRE(fresh);
    CHECK_EQ(tipc_result(f, second, request(101, true)), k::kOk);
    CHECK_EQ(tipc_result(f, fresh, request(101, true)), k::kInvalidState);
    CHECK_EQ(tipc_result(f, fresh, remote_boot(k::current_process()->id)), k::kOk);
}
NXTEST(graphics_rendering_and_transfer_memory_requests_never_return_fake_success) {
    AmFixture f; const auto gsv = f.child(f.child(f.open(), 1), 2000); REQUIRE(gsv);
    for (const auto cmd : {1u, 115u, 201u, 202u, 301u, 497u, 1402u}) {
        CHECK_EQ(tipc_result(f, gsv, request(cmd, true)), k::kNotImplemented);
        const auto reply = f.reply(true); CHECK(reply.copy.empty() && reply.move.empty()); CHECK_EQ(reply.raw.size(), size_t{4});
    }
}
NXTEST(trace_reply_decoder_checks_tipc_cmif_and_domain_results_without_mutating_them) {
    AmFixture f; const auto h = f.connect("test");
    for (bool tipc : {false, true}) {
        REQUIRE(f.send(h, request(8, tipc)) == k::kOk);
        std::array<uint32_t, 64> before; std::memcpy(before.data(), reinterpret_cast<void*>(f.base), sizeof before);
        const auto decoded = nxrt::diagnostic::decode_reply(before.data(), before.size(), tipc);
        CHECK(decoded.valid); CHECK_EQ(decoded.result, k::kInvalidState);
        CHECK(!std::memcmp(before.data(), reinterpret_cast<void*>(f.base), sizeof before));
        before[1] = 0; CHECK(!nxrt::diagnostic::decode_reply(before.data(), before.size(), tipc).valid);
    }
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld; REQUIRE(f.send(h, convert) == k::kOk);
    auto inner = request(8); auto m = request(0); m.raw.clear();
    push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, uint16_t{16});
    push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0}); m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
    REQUIRE(f.send(h, m) == k::kOk);
    const auto decoded = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(f.base), 64, false, 16);
    CHECK(decoded.valid); CHECK_EQ(decoded.result, k::kInvalidState);
}
NXTEST(applet_proxy_requires_current_process_and_supports_cmif_and_tipc) {
    AmFixture f; REQUIRE(f.am);
    for (bool tipc : {false, true}) { const auto p = f.open(tipc); REQUIRE(p); k::handle_close(p); }
    auto m = request(0); push(m.raw, uint64_t{0}); m.has_pid = true;
    CHECK_EQ(f.send(f.am, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidHandle);
    m.copy = {k::handle_add(std::make_shared<k::KEvent>())};
    CHECK_EQ(f.send(f.am, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidHandle);
    m.copy = {k::kPseudoCurrentProcess}; m.has_pid = false;
    CHECK_EQ(f.send(f.am, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidHandle);
}
NXTEST(filesystem_bootstrap_queries_match_the_game_domain_protocol) {
    Fixture f;
    struct Reset { ~Reset() { nxrt::services::fs_reset(); } } reset;
    nxrt::services::fs_reset(); nxrt::services::register_fs();
    const auto fs = f.connect("fsp-srv"); REQUIRE(fs);
    auto bind = request(1); bind.has_pid = true; push(bind.raw, uint64_t{0});
    REQUIRE(f.send(fs, bind) == k::kOk); REQUIRE(word(f.reply().raw, 8) == 0);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(fs, convert) == k::kOk); REQUIRE(word(f.reply().raw, 8) == 0);
    for (uint32_t command : {1005u, 1011u}) {
        const auto inner = request(command); auto m = request(0); m.raw.clear();
        push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, uint16_t{16});
        push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0});
        m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
        REQUIRE(f.send(fs, m) == k::kOk); const auto reply = f.reply();
        CHECK_EQ(word(reply.raw, 16), 0x4f434653u); CHECK_EQ(word(reply.raw, 24), 0u);
        CHECK_EQ(word(reply.raw, 32), 0u);
        if (command == 1011) CHECK_EQ(word(reply.raw, 36), 1u);
        CHECK(reply.copy.empty() && reply.move.empty());
    }
}
NXTEST(trace_allows_real_empty_queue_results_without_weakening_failure_checks) {
    AmFixture f; const auto common = f.child(f.open(), 0); REQUIRE(common);
    CHECK_EQ(f.receive(common), 15u); CHECK_EQ(f.receive(common), UINT32_MAX);
    auto reply = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(f.base), 64, false);
    CHECK_EQ(reply.result, k::make_result(128, 3));
    CHECK(!nxrt::diagnostic::stop_on_reply(reply, "ICommonStateGetter", 1));
    CHECK(nxrt::diagnostic::stop_on_reply(reply, "ICommonStateGetter", 66));
    CHECK(nxrt::diagnostic::stop_on_reply(reply, "OtherService", 1));
    reply.valid = false; CHECK(nxrt::diagnostic::stop_on_reply(reply, "ICommonStateGetter", 1));
    nxrt::services::register_aoc(); const auto aoc = f.connect("aoc:u"), purchase = f.child(aoc, 100); REQUIRE(purchase);
    for (uint32_t command : {3u, 4u}) {
        REQUIRE(f.send(purchase, request(command)) == k::kOk);
        reply = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(f.base), 64, false);
        CHECK_EQ(reply.result, k::make_result(164, 400));
        CHECK(!nxrt::diagnostic::stop_on_reply(reply, "IPurchaseEventManager", command));
        CHECK(nxrt::diagnostic::stop_on_reply(reply, "IPurchaseEventManager", 2));
    }
    for (auto result : {k::kNotImplemented, k::kStubService, k::kInvalidSize, k::kOutOfRange, uint32_t{0x202}}) {
        reply = {true, result, 0x4f434653};
        CHECK(nxrt::diagnostic::stop_on_reply(reply, "ICommonStateGetter", 1));
        CHECK(nxrt::diagnostic::stop_on_reply(reply, "IPurchaseEventManager", 3));
        CHECK(nxrt::diagnostic::stop_on_reply(reply, "fsp-srv", 200));
    }
    CHECK(!nxrt::diagnostic::stop_on_reply({true, 0, 0x4f434653}, "ICommonStateGetter", 66));
}
NXTEST(trace_argument_snapshot_preserves_wire_values_before_tls_is_overwritten) {
    const auto decoded = [](const hipc::Message& input) {
        std::array<uint32_t, 64> words{}; hipc::Message output;
        if (!hipc::build(input, words.data(), words.size()) || !hipc::parse(words.data(), words.size(), output)) std::abort();
        return output;
    };
    auto tipc = request(66, true); push(tipc.raw, uint32_t{2});
    auto args = nxrt::diagnostic::snapshot_arguments(decoded(tipc), false);
    CHECK(args.valid && args.exact_size); CHECK_EQ(args.bytes, size_t{4}); CHECK_EQ(args.captured, size_t{4});
    CHECK_EQ(args.prefix[0], uint8_t{2});
    auto inner = request(66); push(inner.raw, uint32_t{2});
    args = nxrt::diagnostic::snapshot_arguments(decoded(inner), false);
    CHECK(args.valid && !args.exact_size); CHECK(args.bytes >= 4); CHECK_EQ(args.prefix[0], uint8_t{2});
    auto domain = request(0); domain.raw.clear();
    push(domain.raw, uint8_t{1}); push(domain.raw, uint8_t{0}); push(domain.raw, uint16_t{20});
    push(domain.raw, uint32_t{1}); push(domain.raw, uint64_t{0});
    domain.raw.insert(domain.raw.end(), inner.raw.begin(), inner.raw.end());
    auto message = decoded(domain); const auto before = message.raw;
    args = nxrt::diagnostic::snapshot_arguments(message, true);
    CHECK(args.valid && args.exact_size); CHECK_EQ(args.bytes, size_t{4}); CHECK_EQ(args.captured, size_t{4});
    CHECK(message.raw == before); message.raw.assign(message.raw.size(), 0xff);
    CHECK_EQ(args.prefix[0], uint8_t{2}); CHECK_EQ(args.prefix[1], uint8_t{0});
    auto long_input = request(1, true); long_input.raw.resize(80, 0xa5);
    args = nxrt::diagnostic::snapshot_arguments(long_input, false);
    CHECK(args.valid && args.exact_size); CHECK_EQ(args.bytes, size_t{80}); CHECK_EQ(args.captured, size_t{16});
    CHECK(std::all_of(args.prefix.begin(), args.prefix.end(), [](uint8_t value) { return value == 0xa5; }));
}
NXTEST(trace_argument_snapshot_refuses_truncated_headers_and_domain_close) {
    for (size_t size : {0u, 1u, 15u, 16u, 31u}) {
        auto m = request(0); m.raw.resize(size, 0);
        CHECK(!nxrt::diagnostic::snapshot_arguments(m, true).valid);
    }
    auto m = request(0); m.raw.resize(40, 0); m.raw[0] = 1;
    uint16_t size = 25; std::memcpy(m.raw.data() + 2, &size, 2);
    CHECK(!nxrt::diagnostic::snapshot_arguments(m, true).valid);
    size = 16; std::memcpy(m.raw.data() + 2, &size, 2);
    CHECK(!nxrt::diagnostic::snapshot_arguments(m, true).valid);
    const uint32_t magic = 0x49434653; std::memcpy(m.raw.data() + 16, &magic, 4);
    const auto args = nxrt::diagnostic::snapshot_arguments(m, true);
    CHECK(args.valid && args.exact_size); CHECK_EQ(args.bytes, size_t{0}); CHECK_EQ(args.captured, size_t{0});
    m.raw[0] = 2; CHECK(!nxrt::diagnostic::snapshot_arguments(m, true).valid);
    m = request(0); m.raw[0] = 0; CHECK(!nxrt::diagnostic::snapshot_arguments(m, false).valid);
    m = request(0); m.type = hipc::kInvoke2ManagerMethodOld;
    CHECK(!nxrt::diagnostic::snapshot_arguments(m, false).valid);
}
NXTEST(applet_messages_signal_shared_events_and_follow_notification_flags) {
    AmFixture f; const auto proxy = f.open(); REQUIRE(proxy);
    const auto common = f.child(proxy, 0), self = f.child(proxy, 1); REQUIRE(common && self);
    CHECK_EQ(f.send(common, request(0)), k::kOk); auto r = f.reply(); REQUIRE(r.copy.size() == 1);
    auto event = k::handle_as<k::KEvent>(r.copy[0]); REQUIRE(event); CHECK(!event->writable); CHECK(event->signaled());
    CHECK_EQ(f.receive(common), 15u); CHECK(!event->signaled());
    CHECK_EQ(f.receive(common), UINT32_MAX); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 3));
    nxrt::services::amctl::set_docked(false); CHECK(!event->signaled());
    for (unsigned cmd : {11, 12}) { auto m = request(cmd); push(m.raw, uint8_t{1}); CHECK_EQ(f.send(self, m), k::kOk); }
    nxrt::services::amctl::set_docked(true); CHECK(event->signaled());
    CHECK_EQ(f.receive(common), 30u); CHECK(event->signaled()); CHECK_EQ(f.receive(common), 31u); CHECK(!event->signaled());
    nxrt::services::amctl::set_focus(false); CHECK(event->signaled()); CHECK_EQ(f.receive(common), 15u);
    CHECK_EQ(f.send(common, request(9)), k::kOk); CHECK_EQ(word(f.reply().raw, 16) & 0xffu, 2u);
    CHECK_EQ(f.send(common, request(130)), k::kOk); CHECK_EQ(f.receive(common), 95u); CHECK(!event->signaled());
}
NXTEST(applet_reset_keeps_old_sessions_isolated_and_unported_commands_fail) {
    AmFixture f; const auto p = f.open(); REQUIRE(p); const auto old = f.child(p, 0); REQUIRE(old);
    CHECK_EQ(f.receive(old), 15u); nxrt::services::am_reset();
    const auto next = f.open(); REQUIRE(next); const auto fresh = f.child(next, 0); REQUIRE(fresh);
    nxrt::services::amctl::set_focus(false);
    CHECK_EQ(f.receive(old), UINT32_MAX); CHECK_EQ(f.receive(fresh), 15u); CHECK_EQ(f.receive(fresh), 15u);
    CHECK_EQ(f.send(p, request(999)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kStubService);
    CHECK(!ipc::has_service("appletAE"));
}
NXTEST(applet_graphics_scale_reports_the_fixed_guest_profile) {
    AmFixture f; const auto p = f.open(); REQUIRE(p); const auto scale = f.child(p, 60); REQUIRE(scale);
    CHECK_EQ(f.send(scale, request(10)), k::kOk); auto reply = f.reply();
    CHECK_EQ(word(reply.raw, 8), 0u); CHECK_EQ(word(reply.raw, 16), 10u);
    CHECK_EQ(word(reply.raw, 20), 0u); CHECK_EQ(word(reply.raw, 24), 0u); CHECK_EQ(word(reply.raw, 28), 0u);
    CHECK_EQ(f.send(scale, request(999)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kStubService);
}
NXTEST(applet_performance_service_cannot_claim_host_overclocking) {
    AmFixture f; const auto p = f.open(); REQUIRE(p); const auto performance = f.child(p, 40); REQUIRE(performance);
    CHECK_EQ(f.send(performance, request(0)), k::kOk); CHECK_EQ(word(f.reply().raw, 16) & 0xffu, 0u);
    auto m = request(1); push(m.raw, uint8_t{1}); CHECK_EQ(f.send(performance, m), k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::kNotImplemented);
    m = request(1); push(m.raw, uint8_t{0}); CHECK_EQ(f.send(performance, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
}
NXTEST(applet_storage_accessors_share_bytes_and_outlive_the_storage_session) {
    AmFixture f; const auto p = f.open(), creator = f.child(p, 11); REQUIRE(creator);
    const auto storage = f.storage(creator, 16); REQUIRE(storage);
    const auto first = f.child(storage, 0), second = f.child(storage, 0); REQUIRE(first && second);
    k::handle_close(storage); k::handle_close(creator); k::handle_close(p);
    CHECK_EQ(f.send(first, request(0)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 16u);
    const auto data = f.base + 0x1000; std::memcpy(reinterpret_cast<void*>(data), "PS5!", 4);
    auto m = request(10); push(m.raw, int64_t{4}); m.send.push_back({data, 4, 0});
    CHECK_EQ(f.send(first, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    k::handle_close(first); std::memset(reinterpret_cast<void*>(data), 0xff, 16);
    m = request(11); push(m.raw, int64_t{0}); m.recv.push_back({data, 16, 0});
    CHECK_EQ(f.send(second, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    const std::array<uint8_t, 16> expected{0, 0, 0, 0, 'P', 'S', '5', '!'};
    CHECK(std::memcmp(reinterpret_cast<void*>(data), expected.data(), expected.size()) == 0);
}
NXTEST(applet_loading_hint_is_shared_cancellable_and_reset_per_application) {
    AmFixture f; const auto proxy = f.open();
    const auto first = f.child(proxy, 0), second = f.child(proxy, 0); REQUIRE(first && second);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
    for (uint32_t mode : {2u, 0u, 1u}) {
        auto m = request(66); push(m.raw, mode);
        REQUIRE(f.send(first, m) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
        CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, mode);
    }
    for (uint32_t mode : {3u, UINT32_MAX}) {
        auto m = request(66); push(m.raw, mode);
        REQUIRE(f.send(second, m) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kOutOfRange);
        CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 1u);
    }
    REQUIRE(f.send(second, request(67)) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
    auto m = request(66); push(m.raw, uint32_t{1}); REQUIRE(f.send(first, m) == k::kOk);
    nxrt::services::am_reset(); CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
    REQUIRE(f.send(first, m) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
}
NXTEST(applet_loading_hint_accepts_the_supplied_sdk_domain_wire_value) {
    AmFixture f; const auto common = f.child(f.open(), 0); REQUIRE(common);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(common, convert) == k::kOk); REQUIRE(word(f.reply().raw, 8) == 0);
    const auto invoke = [&](uint32_t command, uint32_t mode) {
        auto inner = request(command);
        if (command == 66) push(inner.raw, mode);
        auto m = request(0); m.raw.clear();
        push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, static_cast<uint16_t>(inner.raw.size()));
        push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0});
        m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
        return m;
    };
    // nn::oe::SetCpuBoostMode(1) maps to applet command 66 with u32 value 2
    // in this title's SDK. Exercise the domain envelope used by the real call.
    for (uint32_t mode : {2u, 0u, 2u}) {
        REQUIRE(f.send(common, invoke(66, mode)) == k::kOk);
        const auto reply = f.reply(); CHECK_EQ(word(reply.raw, 24), 0u);
        CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, mode);
    }
    REQUIRE(f.send(common, invoke(66, 3)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), k::kOutOfRange);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 2u);
    REQUIRE(f.send(common, invoke(67, 0)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), 0u);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
}
NXTEST(applet_loading_hint_rejects_short_inputs_and_foreign_resources_without_mutation) {
    AmFixture f; const auto common = f.child(f.open(), 0); REQUIRE(common);
    const auto service = k::handle_as<ipc::KSession>(common)->svc;
    uint32_t mode = 1; ipc::Request req; ipc::Response rsp;
    req.cmd = 66; req.in = reinterpret_cast<const uint8_t*>(&mode);
    for (size_t size : {0u, 1u, 2u, 3u}) {
        req.in_size = size; CHECK_EQ(service->handle(req, rsp), k::kInvalidSize);
    }
    req.in_size = 4; req.in = nullptr; CHECK_EQ(service->handle(req, rsp), k::kInvalidSize);
    req.in = reinterpret_cast<const uint8_t*>(&mode);
    for (auto member : {&ipc::Request::in_copy, &ipc::Request::in_move}) {
        (req.*member).push_back(k::kPseudoCurrentProcess);
        CHECK_EQ(service->handle(req, rsp), k::kInvalidHandle); (req.*member).clear();
    }
    for (auto member : {&ipc::Request::a, &ipc::Request::b, &ipc::Request::w, &ipc::Request::x, &ipc::Request::c}) {
        (req.*member).push_back({f.base, 4});
        CHECK_EQ(service->handle(req, rsp), k::kInvalidCombination); (req.*member).clear();
    }
    req.in_services.push_back(service); CHECK_EQ(service->handle(req, rsp), k::kInvalidCombination);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 0u);
    CHECK(rsp.out.empty() && rsp.out_copy.empty() && rsp.out_move.empty());
    req.in_services.clear(); REQUIRE(service->handle(req, rsp) == k::kOk);
    req.cmd = 67; req.in_copy.push_back(k::kPseudoCurrentProcess);
    CHECK_EQ(service->handle(req, rsp), k::kInvalidHandle);
    CHECK_EQ(nxrt::services::amctl::startup_state().cpu_boost_mode, 1u);
}
NXTEST(applet_resolution_comes_from_renderer_and_change_events_share_real_state) {
    AmFixture f; const auto common = f.child(f.open(), 0); REQUIRE(common);
    REQUIRE(f.send(common, request(60)) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidState);
    REQUIRE(f.send(common, request(61)) == k::kOk); auto reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto first = reply.copy[0]; auto event = k::handle_as<k::KEvent>(first); REQUIRE(event);
    CHECK(!event->writable && !event->signaled());
    REQUIRE(f.send(common, request(61)) == k::kOk); reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto second = reply.copy[0]; CHECK(k::handle_as<k::KEvent>(second)->state == event->state);
    CHECK(!nxrt::services::amctl::set_display_resolution(0, 2160));
    CHECK(!nxrt::services::amctl::set_display_resolution(3840, -1));
    REQUIRE(nxrt::services::amctl::set_display_resolution(3840, 2160)); CHECK(!event->signaled());
    REQUIRE(f.send(common, request(60)) == k::kOk); reply = f.reply();
    CHECK_EQ(word(reply.raw, 8), 0u); CHECK_EQ(word(reply.raw, 16), 3840u); CHECK_EQ(word(reply.raw, 20), 2160u);
    REQUIRE(nxrt::services::amctl::set_display_resolution(1920, 1080)); CHECK(event->signaled());
    k::handle_close(first); f.c.x[0] = second; CHECK_EQ(call(f.c, 0x17), k::kOk);
    REQUIRE(nxrt::services::amctl::set_display_resolution(1920, 1080)); CHECK(!event->signaled());
    CHECK(!nxrt::services::amctl::set_display_resolution(-1, 1080)); CHECK(!event->signaled());
    nxrt::services::am_reset(); REQUIRE(nxrt::services::amctl::set_display_resolution(1280, 720));
    CHECK(!event->signaled()); REQUIRE(f.send(common, request(60)) == k::kOk); reply = f.reply();
    CHECK_EQ(word(reply.raw, 16), 1920u); CHECK_EQ(word(reply.raw, 20), 1080u);
    const auto fresh = f.child(f.open(), 0); REQUIRE(fresh);
    REQUIRE(f.send(fresh, request(60)) == k::kOk); reply = f.reply();
    CHECK_EQ(word(reply.raw, 16), 1280u); CHECK_EQ(word(reply.raw, 20), 720u);
}
NXTEST(applet_running_notification_is_idempotent_and_does_not_cross_reset) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    CHECK(!nxrt::services::amctl::startup_state().notified_running);
    auto malformed = request(40); malformed.send.push_back({f.base + 0x1000, 4, 0});
    REQUIRE(f.send(application, malformed) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kInvalidCombination);
    CHECK(!nxrt::services::amctl::startup_state().notified_running);
    for (unsigned n = 0; n != 2; ++n) {
        REQUIRE(f.send(application, request(40)) == k::kOk); const auto reply = f.reply();
        CHECK_EQ(word(reply.raw, 8), 0u); CHECK_EQ(word(reply.raw, 16) & 0xffu, 1u);
        CHECK(nxrt::services::amctl::startup_state().notified_running);
    }
    nxrt::services::am_reset(); REQUIRE(f.send(application, request(40)) == k::kOk);
    CHECK(!nxrt::services::amctl::startup_state().notified_running);
}
NXTEST(applet_storage_pointer_buffers_round_trip_and_invalid_offsets_do_not_partially_write) {
    AmFixture f; const auto p = f.open(), creator = f.child(p, 11), storage = f.storage(creator, 8);
    const auto accessor = f.child(storage, 0); REQUIRE(accessor); const auto data = f.base + 0x1000;
    std::memcpy(reinterpret_cast<void*>(data), "abcdefgh", 8);
    auto m = request(10); push(m.raw, int64_t{0}); m.ptrs.push_back({data, 8, 0});
    CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    std::memset(reinterpret_cast<void*>(data), 0xff, 8);
    for (auto offset : {int64_t{-1}, int64_t{7}, INT64_MAX}) {
        m = request(10); push(m.raw, offset); m.send.push_back({data, 2, 0});
        CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 503));
        m = request(11); push(m.raw, offset); m.recv.push_back({data, 2, 0});
        CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 503));
        CHECK_EQ(*reinterpret_cast<uint64_t*>(data), UINT64_MAX);
    }
    m = request(11); push(m.raw, int64_t{0}); m.recv_list_flags = 2; m.recv_list.push_back({data, 8});
    CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    CHECK(std::memcmp(reinterpret_cast<void*>(data), "abcdefgh", 8) == 0);
}
NXTEST(applet_storage_creation_limits_and_empty_storage_have_defined_results) {
    AmFixture f; const auto p = f.open(), creator = f.child(p, 11); REQUIRE(creator);
    const auto count = k::handle_count();
    for (auto size : {int64_t{-1}, int64_t{0x10000001}, INT64_MAX}) {
        CHECK_EQ(f.storage(creator, size), 0u); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 600));
        CHECK_EQ(k::handle_count(), count);
    }
    const auto storage = f.storage(creator, 0), accessor = f.child(storage, 0); REQUIRE(accessor);
    CHECK_EQ(f.send(accessor, request(0)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0u);
    for (auto cmd : {10u, 11u}) {
        auto m = request(cmd); push(m.raw, int64_t{0});
        CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
        m = request(cmd); push(m.raw, int64_t{1});
        CHECK_EQ(f.send(accessor, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 503));
    }
}
NXTEST(applet_storage_respects_guest_permissions_and_rejects_unported_applet_backing) {
    AmFixture f; const auto p = f.open(), creator = f.child(p, 11), storage = f.storage(creator, 8);
    const auto accessor = f.child(storage, 0); REQUIRE(accessor); const auto data = f.base + 0x1000;
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::R));
    auto m = request(11); push(m.raw, int64_t{0}); m.recv.push_back({data, 8, 0});
    CHECK_EQ(f.send(accessor, m), k::kInvalidMemoryState);
    REQUIRE(mem::protect(data, 0x1000, mem::Prot::None));
    m = request(10); push(m.raw, int64_t{0}); m.send.push_back({data, 8, 0});
    CHECK_EQ(f.send(accessor, m), k::kInvalidMemoryState);
    // Direct dispatch is also protected, even outside the checked SVC entry point.
    int64_t offset = 0; ipc::Request req; ipc::Response rsp; req.cmd = 10;
    req.in = reinterpret_cast<uint8_t*>(&offset); req.in_size = sizeof offset; req.a.push_back({data, 8});
    CHECK_EQ(k::handle_as<ipc::KSession>(accessor)->svc->handle(req, rsp), k::kInvalidMemoryState);
    for (auto cmd : {0u, 3u, 11u, 12u}) {
        CHECK_EQ(f.send(creator, request(cmd)), k::kOk); const auto r = f.reply();
        CHECK_EQ(word(r.raw, 8), k::kNotImplemented); CHECK(r.move.empty());
    }
}
NXTEST(applet_expected_audio_volumes_are_shared_clamped_and_reset_separately) {
    AmFixture f; const auto p = f.open(), first = f.child(p, 3), second = f.child(p, 3); REQUIRE(first && second);
    auto m = request(0); push(m.raw, -1.0f); push(m.raw, 2.0f);
    CHECK_EQ(f.send(first, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    CHECK_EQ(f.send(second, request(1)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0u);
    CHECK_EQ(f.send(second, request(2)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0x3f800000u);
    m = request(0); push(m.raw, 0.5f); push(m.raw, std::numeric_limits<float>::quiet_NaN());
    CHECK_EQ(f.send(first, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kOutOfRange);
    CHECK_EQ(f.send(second, request(1)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0u);
    nxrt::services::am_reset(); const auto fresh = f.child(f.open(), 3); REQUIRE(fresh);
    CHECK_EQ(f.send(fresh, request(1)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0x3f800000u);
    CHECK_EQ(f.send(first, request(1)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0u);
    CHECK_EQ(f.send(first, request(3)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kStubService);
}
NXTEST(applet_display_and_debug_endpoints_cannot_report_unbacked_operations_as_success) {
    AmFixture f; const auto p = f.open(); REQUIRE(p);
    const auto display = f.child(p, 4), debug = f.child(p, 1000); REQUIRE(display && debug);
    CHECK_EQ(k::handle_as<ipc::KSession>(display)->svc->name(), std::string("IDisplayController"));
    CHECK_EQ(k::handle_as<ipc::KSession>(debug)->svc->name(), std::string("IDebugFunctions"));
    const auto count = k::handle_count(), data = f.base + 0x1000;
    std::memset(reinterpret_cast<void*>(data), 0xa5, 32);
    for (uint32_t command = 0; command <= 28; ++command) {
        if (command == 19) continue;
        auto m = request(command); m.recv.push_back({data, 32, 0});
        CHECK_EQ(f.send(display, m), k::kOk); auto reply = f.reply();
        CHECK_EQ(word(reply.raw, 8), k::kNotImplemented); CHECK(reply.copy.empty() && reply.move.empty());
        CHECK_EQ(k::handle_count(), count); CHECK_EQ(*reinterpret_cast<uint64_t*>(data), 0xa5a5a5a5a5a5a5a5ull);
    }
    for (auto command : {0u, 10u, 20u, 40u, 100u, 300u}) {
        CHECK_EQ(f.send(debug, request(command)), k::kOk); const auto reply = f.reply();
        CHECK_EQ(word(reply.raw, 8), k::kNotImplemented); CHECK(reply.copy.empty() && reply.move.empty());
    }
    CHECK_EQ(f.send(display, request(999)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::kStubService);
}
NXTEST(applet_lifecycle_policy_updates_atomically_and_old_sessions_stay_isolated) {
    AmFixture f; const auto p = f.open(), self = f.child(p, 1), common = f.child(p, 0); REQUIRE(self && common);
    auto m = request(13); push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, uint8_t{1});
    CHECK_EQ(f.send(self, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    auto policy = nxrt::services::amctl::lifecycle_policy();
    CHECK(policy.focus_handling[0] && !policy.focus_handling[1] && policy.focus_handling[2]);
    const uint8_t short_args[] = {0, 1}; ipc::Request req; ipc::Response rsp;
    req.cmd = 13; req.in = short_args; req.in_size = sizeof short_args;
    CHECK_EQ(k::handle_as<ipc::KSession>(self)->svc->handle(req, rsp), k::kInvalidCombination);
    CHECK(nxrt::services::amctl::lifecycle_policy().focus_handling == policy.focus_handling);
    m = request(14); push(m.raw, uint8_t{1}); CHECK_EQ(f.send(self, m), k::kOk);
    m = request(16); push(m.raw, uint8_t{0}); CHECK_EQ(f.send(self, m), k::kOk);
    m = request(900); CHECK_EQ(f.send(common, m), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    policy = nxrt::services::amctl::lifecycle_policy(); CHECK(policy.restart_message && !policy.out_of_focus_suspending);
    CHECK(policy.exit_library_on_next_program);
    nxrt::services::am_reset(); CHECK_EQ(f.send(common, m), k::kOk);
    policy = nxrt::services::amctl::lifecycle_policy(); CHECK(!policy.restart_message && policy.out_of_focus_suspending);
    CHECK(!policy.exit_library_on_next_program);
    CHECK(!policy.focus_handling[0] && !policy.focus_handling[1] && !policy.focus_handling[2]);
}
NXTEST(applet_suspended_tick_accounting_signals_shared_events_and_optional_resume_messages) {
    AmFixture f; const auto p = f.open(), self = f.child(p, 1), common = f.child(p, 0); REQUIRE(self && common);
    CHECK_EQ(f.receive(common), 15u);
    CHECK_EQ(f.send(self, request(91)), k::kOk); auto reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto first = reply.copy[0]; auto event = k::handle_as<k::KEvent>(first); REQUIRE(event); CHECK(!event->signaled());
    CHECK_EQ(f.send(self, request(91)), k::kOk); reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto second = reply.copy[0]; CHECK(k::handle_as<k::KEvent>(second)->state == event->state);
    CHECK(nxrt::services::amctl::record_suspended_ticks(0)); CHECK(!event->signaled());
    CHECK(nxrt::services::amctl::record_suspended_ticks(0x100000002ull)); CHECK(event->signaled());
    CHECK_EQ(f.receive(common), UINT32_MAX);
    CHECK_EQ(f.send(self, request(90)), k::kOk); reply = f.reply();
    CHECK_EQ(word(reply.raw, 16), 2u); CHECK_EQ(word(reply.raw, 20), 1u);
    f.c.x[0] = second; CHECK_EQ(call(f.c, 0x17), k::kOk); CHECK(!event->signaled());
    k::handle_close(first);
    auto m = request(14); push(m.raw, uint8_t{1}); CHECK_EQ(f.send(self, m), k::kOk);
    CHECK(nxrt::services::amctl::record_suspended_ticks(3)); CHECK(event->signaled()); CHECK_EQ(f.receive(common), 16u);
    CHECK_EQ(f.send(self, request(90)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 5u);
}
NXTEST(applet_suspension_overflow_and_reset_preserve_previous_state_and_events) {
    AmFixture f; const auto self = f.child(f.open(), 1); REQUIRE(self);
    CHECK_EQ(f.send(self, request(91)), k::kOk); auto reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto handle = reply.copy[0]; auto event = k::handle_as<k::KEvent>(handle); REQUIRE(event);
    CHECK(nxrt::services::amctl::record_suspended_ticks(UINT64_MAX));
    f.c.x[0] = handle; CHECK_EQ(call(f.c, 0x17), k::kOk);
    CHECK(!nxrt::services::amctl::record_suspended_ticks(1)); CHECK(!event->signaled());
    CHECK_EQ(f.send(self, request(90)), k::kOk); reply = f.reply();
    CHECK_EQ(word(reply.raw, 16), UINT32_MAX); CHECK_EQ(word(reply.raw, 20), UINT32_MAX);
    nxrt::services::am_reset(); CHECK(nxrt::services::amctl::record_suspended_ticks(7)); CHECK(!event->signaled());
    CHECK_EQ(f.send(self, request(90)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), UINT32_MAX);
    const auto next = f.child(f.open(), 1); REQUIRE(next);
    CHECK_EQ(f.send(next, request(90)), k::kOk); CHECK_EQ(word(f.reply().raw, 16), 7u);
}
NXTEST(applet_fatal_sections_nest_and_reject_unbalanced_leaves) {
    AmFixture f; const auto self = f.child(f.open(), 1); REQUIRE(self);
    CHECK_EQ(f.send(self, request(4)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 512));
    for (auto command : {3u, 3u, 4u, 4u}) {
        CHECK_EQ(f.send(self, request(command)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    }
    CHECK_EQ(f.send(self, request(4)), k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 512));
}
NXTEST(applet_gpu_error_event_copies_remain_shared_and_reset_isolates_old_readers) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    CHECK_EQ(f.send(application, request(130)), k::kOk); auto reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto first = reply.copy[0]; auto event = k::handle_as<k::KEvent>(first); REQUIRE(event);
    CHECK(!event->writable && !event->signaled());
    CHECK_EQ(f.send(application, request(130)), k::kOk); reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    const auto second = reply.copy[0]; CHECK(k::handle_as<k::KEvent>(second)->state == event->state);
    k::handle_close(first); nxrt::services::amctl::signal_gpu_error(); CHECK(event->signaled());
    f.c.x[0] = second; CHECK_EQ(call(f.c, 0x17), k::kOk); CHECK(!event->signaled());
    nxrt::services::am_reset(); nxrt::services::amctl::signal_gpu_error(); CHECK(!event->signaled());
    const auto next = f.child(f.open(), 20); REQUIRE(next);
    CHECK_EQ(f.send(next, request(130)), k::kOk); reply = f.reply(); REQUIRE(reply.copy.size() == 1);
    CHECK(k::handle_as<k::KEvent>(reply.copy[0])->signaled());
}
NXTEST(applet_offline_events_use_shared_readable_handles_and_isolated_channels) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(application, convert) == k::kOk);
    std::vector<std::shared_ptr<k::KEvent>> events;
    for (auto command : {140u, 150u, 160u, 210u}) {
        const auto inner = request(command); auto m = request(0); m.raw.clear();
        push(m.raw, uint8_t{1}); push(m.raw, uint8_t{0}); push(m.raw, uint16_t{16});
        push(m.raw, uint32_t{1}); push(m.raw, uint64_t{0});
        m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end());
        REQUIRE(f.send(application, m) == k::kOk); auto r = f.reply();
        CHECK_EQ(word(r.raw, 16), 0x4f434653u); CHECK_EQ(word(r.raw, 24), 0u);
        REQUIRE(r.copy.size() == 1); CHECK(r.move.empty()); CHECK_EQ(word(r.raw, 0), 0u);
        auto first = k::handle_as<k::KEvent>(r.copy[0]); REQUIRE(first);
        CHECK(!first->writable && !first->signaled()); k::handle_close(r.copy[0]);
        REQUIRE(f.send(application, m) == k::kOk); r = f.reply(); REQUIRE(r.copy.size() == 1);
        CHECK(k::handle_as<k::KEvent>(r.copy[0])->state == first->state);
        for (const auto& previous : events) CHECK(first->state != previous->state);
        events.push_back(first);
    }
    nxrt::services::amctl::signal_gpu_error();
    for (const auto& event : events) CHECK(!event->signaled());
    nxrt::services::am_reset(); const auto fresh = f.child(f.open(), 20); REQUIRE(fresh);
    REQUIRE(f.send(fresh, request(210)) == k::kOk); const auto r = f.reply(); REQUIRE(r.copy.size() == 1);
    CHECK(k::handle_as<k::KEvent>(r.copy[0])->state != events.back()->state);
}
NXTEST(applet_event_and_metadata_queries_reject_foreign_resources_without_handle_leaks) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    const auto service = k::handle_as<ipc::KSession>(application)->svc; const auto count = k::handle_count();
    for (auto command : {21u, 23u, 65u, 121u, 123u, 130u, 140u, 141u, 150u, 151u, 160u, 210u}) {
        ipc::Request req; req.cmd = command; ipc::Response rsp;
        req.in_copy = {k::kPseudoCurrentProcess}; CHECK_EQ(service->handle(req, rsp), k::kInvalidHandle);
        req.in_copy.clear(); req.a = {{f.base, 4}}; CHECK_EQ(service->handle(req, rsp), k::kInvalidCombination);
        req.a.clear(); req.in_services = {service}; CHECK_EQ(service->handle(req, rsp), k::kInvalidCombination);
        CHECK(rsp.out.empty() && rsp.out_copy.empty() && rsp.out_objects.empty()); CHECK_EQ(k::handle_count(), count);
    }
}
NXTEST(applet_event_handle_exhaustion_fails_without_losing_service_state) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    const auto service = k::handle_as<ipc::KSession>(application)->svc;
    auto object = std::make_shared<k::KEvent>(); std::vector<uint32_t> handles;
    for (;;) { auto h = k::handle_add(object); if (!h) break; handles.push_back(h); }
    ipc::Request req; req.cmd = 210; ipc::Response rsp;
    CHECK_EQ(service->handle(req, rsp), k::kOutOfHandles); CHECK(rsp.out_copy.empty()); REQUIRE(!handles.empty());
    k::handle_close(handles.back()); handles.pop_back();
    CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out_copy.size() == 1);
    CHECK(!k::handle_as<k::KEvent>(rsp.out_copy[0])->signaled());
    for (auto h : handles) k::handle_close(h);
}
NXTEST(applet_empty_launch_and_notification_channels_reach_the_sdk_unchanged) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    const auto count = k::handle_count();
    for (auto command : {1u, 141u, 151u}) {
        auto m = request(command); if (command == 1) push(m.raw, uint32_t{1});
        REQUIRE(f.send(application, m) == k::kOk); auto r = f.reply();
        CHECK_EQ(word(r.raw, 8), k::make_result(128, 2)); CHECK(r.copy.empty() && r.move.empty());
        const auto decoded = nxrt::diagnostic::decode_reply(reinterpret_cast<const uint32_t*>(f.base), 64, false);
        CHECK(!nxrt::diagnostic::stop_on_reply(decoded, "IApplicationFunctions", command));
        CHECK(nxrt::diagnostic::stop_on_reply(decoded, "IApplicationFunctions", 210));
        CHECK(nxrt::diagnostic::stop_on_reply(decoded, "OtherService", command));
        CHECK(nxrt::diagnostic::stop_on_reply({false, decoded.result, 0}, "IApplicationFunctions", command));
        CHECK(nxrt::diagnostic::stop_on_reply({true, k::kInvalidHandle, 0x4f434653}, "IApplicationFunctions", command));
    }
    auto m = request(1); push(m.raw, uint32_t{2}); REQUIRE(f.send(application, m) == k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 2)); CHECK_EQ(k::handle_count(), count);
    m = request(1); push(m.raw, uint32_t{3}); REQUIRE(f.send(application, m) == k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::kOutOfRange);
    ipc::Request req; ipc::Response rsp; req.cmd = 1;
    CHECK_EQ(k::handle_as<ipc::KSession>(application)->svc->handle(req, rsp), k::kInvalidSize);
}
NXTEST(applet_user_channel_preserves_copies_lifo_order_and_clear) {
    AmFixture f; const auto p = f.open(), application = f.child(p, 20), creator = f.child(p, 11);
    REQUIRE(application && creator); const auto data = f.base + 0x1000;
    const auto storage = f.storage(creator, 4), accessor = f.child(storage, 0); REQUIRE(accessor);
    for (const char* text : {"one!", "two!"}) {
        std::memcpy(reinterpret_cast<void*>(data), text, 4);
        auto m = request(10); push(m.raw, int64_t{0}); m.send.push_back({data, 4, 0});
        REQUIRE(f.send(accessor, m) == k::kOk); REQUIRE(word(f.reply().raw, 8) == 0);
        // Move a second session for the same storage. The retained accessor
        // mutates the source later, so the channel must keep a byte snapshot.
        const auto duplicate = ipc::create_session_handle(k::handle_as<ipc::KSession>(storage)->svc);
        REQUIRE(duplicate); m = request(122); m.move = {duplicate};
        REQUIRE(f.send(application, m) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
        CHECK(!k::handle_get(duplicate));
    }
    for (const char* expected : {"two!", "one!"}) {
        auto m = request(1); push(m.raw, uint32_t{1}); REQUIRE(f.send(application, m) == k::kOk);
        auto r = f.reply(); REQUIRE(r.move.size() == 1); const auto reader = f.child(r.move[0], 0); REQUIRE(reader);
        std::memset(reinterpret_cast<void*>(data), 0, 4);
        m = request(11); push(m.raw, int64_t{0}); m.recv.push_back({data, 4, 0});
        REQUIRE(f.send(reader, m) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
        CHECK(std::memcmp(reinterpret_cast<void*>(data), expected, 4) == 0);
    }
    auto m = request(122); m.move = {storage}; REQUIRE(f.send(application, m) == k::kOk);
    REQUIRE(f.send(application, request(121)) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    m = request(1); push(m.raw, uint32_t{1}); REQUIRE(f.send(application, m) == k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 2));
}
NXTEST(applet_user_channel_validates_domain_objects_and_bounds_queued_storage) {
    AmFixture f; const auto p = f.open(); REQUIRE(p);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(p, convert) == k::kOk);
    const auto invoke = [&](uint32_t target, hipc::Message inner, uint32_t object = 0) {
        auto m = request(0); m.raw.clear();
        push(m.raw, uint8_t{1}); push(m.raw, static_cast<uint8_t>(object != 0));
        push(m.raw, static_cast<uint16_t>(inner.raw.size()));
        push(m.raw, target); push(m.raw, uint64_t{0});
        m.raw.insert(m.raw.end(), inner.raw.begin(), inner.raw.end()); if (object) push(m.raw, object); return m;
    };
    REQUIRE(f.send(p, invoke(1, request(20))) == k::kOk); const auto application = word(f.reply().raw, 32);
    REQUIRE(f.send(p, invoke(1, request(11))) == k::kOk); const auto creator = word(f.reply().raw, 32);
    auto create = request(10); push(create.raw, int64_t{0});
    REQUIRE(f.send(p, invoke(creator, create)) == k::kOk); const auto storage = word(f.reply().raw, 32);
    REQUIRE(application != UINT32_MAX && creator != UINT32_MAX && storage != UINT32_MAX);
    for (auto object : {1u, 999u}) {
        REQUIRE(f.send(p, invoke(application, request(122), object)) == k::kOk);
        CHECK_EQ(word(f.reply().raw, 24), k::make_result(128, 511));
    }
    for (unsigned n = 0; n < 16; ++n) {
        REQUIRE(f.send(p, invoke(application, request(122), storage)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), 0u);
    }
    REQUIRE(f.send(p, invoke(application, request(122), storage)) == k::kOk);
    CHECK_EQ(word(f.reply().raw, 24), k::make_result(128, 600));
    nxrt::services::am_reset(); const auto fresh = f.child(f.open(), 20); REQUIRE(fresh);
    auto m = request(1); push(m.raw, uint32_t{1}); REQUIRE(f.send(fresh, m) == k::kOk);
    CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 2));
}
NXTEST(applet_basic_queries_return_native_locale_title_version_and_capabilities) {
    AmFixture f; const auto application = f.child(f.open(), 20); REQUIRE(application);
    struct ProfileGuard {
        const nxrt::game::Info* old = nxrt::game::current(); nxrt::game::Info game{};
        ProfileGuard() {
            nxrt::config_clear(); game.title_defaults = +[](nxrt::services::TitleProfile& p) { p.display_version = "2.0.1a"; };
            nxrt::game::set_current(&game);
        }
        ~ProfileGuard() { nxrt::game::set_current(old); nxrt::config_clear(); }
    } guard;
    auto service = k::handle_as<ipc::KSession>(application)->svc; ipc::Request req; ipc::Response rsp;
    req.cmd = 21; CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out.size() == 8);
    CHECK(std::memcmp(rsp.out.data(), "en-US\0\0\0", 8) == 0);
    req.cmd = 23; rsp = {}; CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out.size() == 16);
    CHECK(std::memcmp(rsp.out.data(), "2.0.1a", 6) == 0); CHECK_EQ(rsp.out[6], 0);
    nxrt::config_set("title", "display_version", "12345678901234567890"); rsp = {};
    CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out.size() == 16);
    CHECK(std::memcmp(rsp.out.data(), "123456789012345\0", 16) == 0);
    req.cmd = 65; rsp = {}; CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out.size() == 1); CHECK_EQ(rsp.out[0], 0);
    req.cmd = 123; rsp = {}; CHECK_EQ(service->handle(req, rsp), 0u); CHECK_EQ(word(rsp.out, 0), UINT32_MAX);
}
namespace {
struct AccountGuard {
    AccountGuard() { nxrt::services::acc_reset(); nxrt::services::register_acc(); }
    ~AccountGuard() { nxrt::services::acc_reset(); }
};
}
NXTEST(account_sdk_domain_initialization_profile_and_receive_list_use_real_wire_layouts) {
    Fixture f; AccountGuard guard; const auto account = f.connect("acc:u0"); REQUIRE(account);
    auto convert = request(0); convert.type = hipc::kInvoke2ManagerMethodOld;
    REQUIRE(f.send(account, convert) == k::kOk); CHECK_EQ(word(f.reply().raw, 16), 1u);
    const auto domain = [](uint32_t target, hipc::Message inner) {
        auto body = std::move(inner.raw); inner.raw.clear();
        push(inner.raw, uint8_t{1}); push(inner.raw, uint8_t{0}); push(inner.raw, static_cast<uint16_t>(body.size()));
        push(inner.raw, target); push(inner.raw, uint64_t{0}); inner.raw.insert(inner.raw.end(), body.begin(), body.end());
        return inner;
    };
    auto init = request(160); init.has_pid = true;
    REQUIRE(f.send(account, domain(1, init)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), k::kInvalidSize);
    push(init.raw, uint64_t{0});
    REQUIRE(f.send(account, domain(1, init)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), 0u);
    auto last = domain(1, request(4)); REQUIRE(f.send(account, last) == k::kOk);
    const auto last_reply = f.reply(); REQUIRE(last_reply.raw.size() >= 48);
    std::array<uint8_t, 16> uid{}; std::memcpy(uid.data(), last_reply.raw.data() + 32, uid.size());
    auto get = request(5); get.raw.insert(get.raw.end(), uid.begin(), uid.end());
    REQUIRE(f.send(account, domain(1, get)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), 0u);
    const auto profile = word(f.reply().raw, 32); REQUIRE(profile > 1 && profile != UINT32_MAX);
    auto full = request(0); full.recv_list_flags = 2; full.recv_list.push_back({f.base + 0x1000, 0x80});
    REQUIRE(f.send(account, domain(profile, full)) == k::kOk);
    const auto response = f.reply(); CHECK_EQ(word(response.raw, 24), 0u); REQUIRE(response.raw.size() >= 88);
    CHECK(std::memcmp(response.raw.data() + 32, uid.data(), uid.size()) == 0);
    CHECK_EQ(*reinterpret_cast<const uint32_t*>(f.base + 0x1004), 1u);
    auto users = request(2); users.recv_list_flags = 2; users.recv_list.push_back({f.base + 0x1100, 0x80});
    REQUIRE(f.send(account, domain(1, users)) == k::kOk); CHECK_EQ(word(f.reply().raw, 24), 0u);
    CHECK(std::memcmp(reinterpret_cast<void*>(f.base + 0x1100), uid.data(), uid.size()) == 0);
}
NXTEST(applet_preselected_user_storage_matches_account_uid_is_consumed_once_and_resets_per_launch) {
    AmFixture f; AccountGuard guard; auto application = f.child(f.open(), 20); REQUIRE(application);
    auto pop = request(1); push(pop.raw, uint32_t{2}); REQUIRE(f.send(application, pop) == k::kOk);
    auto response = f.reply(); CHECK_EQ(word(response.raw, 8), 0u); REQUIRE(response.move.size() == 1);
    const auto accessor = f.child(response.move[0], 0); REQUIRE(accessor);
    REQUIRE(f.send(accessor, request(0)) == k::kOk); CHECK_EQ(word(f.reply().raw, 16), 0x88u);
    auto read = request(11); push(read.raw, int64_t{0}); read.recv.push_back({f.base + 0x1000, 0x88, 0});
    REQUIRE(f.send(accessor, read) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    const auto bytes = reinterpret_cast<const uint8_t*>(f.base + 0x1000);
    uint32_t magic = 0; std::memcpy(&magic, bytes, 4); CHECK_EQ(magic, 0xc79497cau); CHECK_EQ(bytes[4], 1u);
    uint64_t hi = 0, lo = 0; REQUIRE(nxrt::services::accapi::launch_user(&hi, &lo));
    CHECK(std::memcmp(bytes + 8, &hi, 8) == 0); CHECK(std::memcmp(bytes + 16, &lo, 8) == 0);
    CHECK(std::all_of(bytes + 24, bytes + 0x88, [](auto v) { return !v; }));
    REQUIRE(f.send(application, pop) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 2));
    nxrt::services::am_reset(); application = f.child(f.open(), 20); REQUIRE(application);
    REQUIRE(f.send(application, pop) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), 0u);
    nxrt::services::am_reset(); nxrt::services::acc_reset(); application = f.child(f.open(), 20); REQUIRE(application);
    REQUIRE(f.send(application, pop) == k::kOk); CHECK_EQ(word(f.reply().raw, 8), k::make_result(128, 2));
}
NXTEST(applet_capture_policy_disables_guest_capture_and_does_not_enable_unbacked_sharing) {
    AmFixture f; const auto p = f.open(), application = f.child(p, 20), self = f.child(p, 1);
    REQUIRE(application && self);
    auto service = k::handle_as<ipc::KSession>(application)->svc;
    for (const auto command : {320u, 321u}) {
        ipc::Request req; ipc::Response rsp; req.cmd = command;
        CHECK_EQ(service->handle(req, rsp), 0u);
        CHECK_EQ(rsp.out.size(), command == 320 ? 2u : 1u);
        CHECK(std::all_of(rsp.out.begin(), rsp.out.end(), [](uint8_t value) { return value == 0; }));
        CHECK(rsp.out_copy.empty() && rsp.out_move.empty() && rsp.out_objects.empty());
    }
    for (const auto enabled : {uint8_t{0}, uint8_t{1}}) {
        auto m = request(2400); push(m.raw, enabled);
        CHECK_EQ(f.send(self, m), k::kOk);
        CHECK_EQ(word(f.reply().raw, 8), enabled ? k::kNotImplemented : 0u);
    }
}
NXTEST(addon_inventory_is_empty_and_copied_events_survive_session_close) {
    Fixture f; nxrt::services::aoc_reset(); nxrt::services::register_aoc();
    auto service = ipc::make_service("aoc:u"); REQUIRE(service);
    ipc::Request req; ipc::Response first; req.cmd = 8;
    CHECK_EQ(service->handle(req, first), 0u); REQUIRE(first.out_copy.size() == 1);
    auto event = k::handle_as<k::KEvent>(first.out_copy[0]); REQUIRE(event); CHECK(!event->signaled());
    ipc::Response second; req.cmd = 10; CHECK_EQ(service->handle(req, second), 0u); REQUIRE(second.out_copy.size() == 1);
    CHECK(event->state == k::handle_as<k::KEvent>(second.out_copy[0])->state);
    service.reset(); k::handle_close(first.out_copy[0]); CHECK(!k::handle_as<k::KEvent>(second.out_copy[0])->signaled());
    nxrt::services::aoc_reset(); service = ipc::make_service("aoc:u"); REQUIRE(service);
    ipc::Response fresh; req.cmd = 8; CHECK_EQ(service->handle(req, fresh), 0u); REQUIRE(fresh.out_copy.size() == 1);
    CHECK(event->state != k::handle_as<k::KEvent>(fresh.out_copy[0])->state);
    for (const auto command : {0u, 2u}) {
        ipc::Response rsp; req.cmd = command; CHECK_EQ(service->handle(req, rsp), 0u);
        CHECK_EQ(word(rsp.out, 0), 0u);
    }
    for (const auto command : {6u, 7u, 11u, 50u}) {
        ipc::Response rsp; req.cmd = command; CHECK_EQ(service->handle(req, rsp), k::kNotImplemented);
        CHECK(rsp.out_move.empty() && rsp.out_objects.empty());
    }
    nxrt::services::aoc_reset();
}
NXTEST(parental_profile_is_process_bound_and_initialization_is_session_local) {
    Fixture f; nxrt::services::register_pctl(); auto factory = ipc::make_service("pctl"); REQUIRE(factory);
    ipc::Request req; ipc::Response refused; req.cmd = 1;
    CHECK_EQ(factory->handle(req, refused), k::kInvalidHandle); CHECK(refused.out_objects.empty());
    req.pid = k::current_process()->id;
    ipc::Response created; CHECK_EQ(factory->handle(req, created), 0u); REQUIRE(created.out_objects.size() == 1);
    auto service = created.out_objects[0]; ipc::Response rsp; req.cmd = 1031;
    CHECK_EQ(service->handle(req, rsp), k::kInvalidState); CHECK(rsp.out.empty());
    req.cmd = 1; CHECK_EQ(service->handle(req, rsp), 0u);
    req.cmd = 1031; CHECK_EQ(service->handle(req, rsp), 0u); REQUIRE(rsp.out.size() == 1); CHECK_EQ(rsp.out[0], 0u);
    req.cmd = 1203; ipc::Response pin; CHECK_EQ(service->handle(req, pin), k::kStubService);
    req.cmd = 1; ipc::Response fresh; CHECK_EQ(factory->handle(req, fresh), 0u); REQUIRE(fresh.out_objects.size() == 1);
    req.cmd = 1031; ipc::Response query; CHECK_EQ(fresh.out_objects[0]->handle(req, query), k::kInvalidState);
}
