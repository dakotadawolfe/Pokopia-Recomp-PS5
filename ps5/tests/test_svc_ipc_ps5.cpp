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
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nxrt::services { void register_logfault(); void logfault_reset(); uint32_t logfault_destination(); }
namespace nxrt::services { void register_am(); void am_reset(); }
namespace nxrt::services { void register_aoc(); void aoc_reset(); }
namespace nxrt::services { void register_pctl(); }

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
