// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native locale. Settings layouts follow settings.cpp and libnx set.c;
// NS application-language numbering follows Eden's ns/language.h. System and
// application language indices differ, so conversions always go through codes.
#include "services_internal.h"
#include "nxrt/locale_ps5.h"
#include "nxrt/log.h"
#include "nxrt/subpage.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace nxrt::ipc::detail { uint64_t note_unhandled(Service&, uint32_t); }
namespace nxrt::services {
namespace {
using ipc::Request; using ipc::Response; using ipc::Service;
using Ptr = std::shared_ptr<Service>;
constexpr uint32_t language_not_found = res(16, 300), invalid_language = res(105, 625);
constexpr std::array<std::string_view, 18> system_codes{
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"
};
constexpr std::array<std::string_view, 16> application_codes{
    "en-US", "en-GB", "ja", "fr", "de", "es-419", "es", "it", "nl", "fr-CA",
    "pt", "ru", "ko", "zh-Hant", "zh-Hans", "pt-BR"
};
// Fixed en-US frontend policy: prefer English, then the American-English
// fallback order used by Eden, including pt-BR next to Portuguese.
constexpr std::array<uint8_t, 16> preferred{0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 15, 11, 2, 14, 13, 12};
uint64_t code(std::string_view text) {
    uint64_t value = 0; std::memcpy(&value, text.data(), text.size()); return value;
}
class LocaleService final : public Service {
    const std::shared_ptr<kernel::KProcess> process_;
public:
    LocaleService(const char* name, std::shared_ptr<kernel::KProcess> process) : Service(name), process_(std::move(process)) {}
    uint32_t handle(Request& req, Response& rsp) override {
        if (process_ != kernel::current_process()) return kernel::kInvalidHandle;
        if (cmds_.contains(req.cmd)) return Service::handle(req, rsp);
        if (ipc::detail::note_unhandled(*this, req.cmd) == 1)
            NXLOG_W("locale", "%s cmd %u is not ported", name().c_str(), req.cmd);
        return kernel::kNotImplemented;
    }
};
uint32_t request(const Request& req, size_t bytes, bool output = false) {
    if (!req.in_copy.empty() || !req.in_move.empty()) return kernel::kInvalidHandle;
    if (!req.in_services.empty() || !req.a.empty() || !req.x.empty() || !req.w.empty()) return kernel::kInvalidCombination;
    if ((!output && (!req.b.empty() || !req.c.empty())) || req.b.size() > 1 || req.c.size() > 1 ||
        (!req.b.empty() && req.b[0].addr && !req.c.empty() && req.c[0].addr)) return kernel::kInvalidCombination;
    if (req.in_size < bytes || (bytes && !req.in)) return kernel::kInvalidSize;
    return 0;
}
uint32_t write(const ipc::Buffer& out, const void* data, size_t size) {
    if (out.size < size) return kernel::kInvalidSize;
    if (size && (!out.addr || !mem::SubpageMap::allows(out.addr, size, 2))) return kernel::kInvalidMemoryState;
    if (size) std::memcpy(reinterpret_cast<void*>(out.addr), data, size);
    return 0;
}
Ptr settings(const std::shared_ptr<kernel::KProcess>& process) {
    auto s = std::make_shared<LocaleService>("ISettingsServer", process);
    s->add_cmd(0, "GetLanguageCode", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push(locale::native_language_code()); return 0;
    });
    for (auto command : {1u, 5u}) s->add_cmd(command, "GetAvailableLanguageCodes", [command](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0, true)) return rc;
        const auto out = req.out_buf(0);
        if (out.size % 8) return kernel::kInvalidSize;
        const auto count = std::min<uint64_t>(out.size / 8, command == 1 ? 15 : system_codes.size());
        std::array<uint64_t, system_codes.size()> codes{};
        for (size_t i = 0; i < count; ++i) codes[i] = code(system_codes[i]);
        if (const auto rc = write(out, codes.data(), count * 8)) return rc;
        rsp.push<int32_t>(static_cast<int32_t>(count)); return 0;
    });
    s->add_cmd(2, "MakeLanguageCode", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 4)) return rc;
        const auto index = req.arg<uint32_t>(0);
        if (index >= system_codes.size()) return invalid_language;
        rsp.push(code(system_codes[index])); return 0;
    });
    for (auto command : {3u, 6u}) s->add_cmd(command, "GetAvailableLanguageCodeCount", [command](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<int32_t>(command == 3 ? 15 : system_codes.size()); return 0;
    });
    s->add_cmd(4, "GetRegionCode", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<int32_t>(1); return 0; // native frontend uses the USA locale
    });
    s->add_cmd(8, "GetQuestFlag", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 0)) return rc;
        rsp.push<uint8_t>(0); return 0;
    });
    s->add_cmd(11, "GetDeviceNickName", [](Request& req, Response&) -> uint32_t {
        if (const auto rc = request(req, 0, true)) return rc;
        constexpr std::array<char, 0x80> nickname{'P', 'o', 'k', 'o', 'p', 'i', 'a', ' ', 'P', 'S', '5'};
        return write(req.out_buf(0), nickname.data(), nickname.size());
    });
    return s;
}
Ptr application_control(const std::shared_ptr<kernel::KProcess>& process) {
    auto s = std::make_shared<LocaleService>("IReadOnlyApplicationControlDataInterface", process);
    s->add_cmd(1, "GetApplicationDesiredLanguage", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 4)) return rc;
        const auto supported = req.arg<uint32_t>(0);
        for (auto language : preferred) {
            if (!supported || (supported & (1u << language))) { rsp.push(language); return 0; }
        }
        return language_not_found;
    });
    s->add_cmd(2, "ConvertApplicationLanguageToLanguageCode", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 1)) return rc;
        const auto language = req.arg<uint8_t>(0);
        if (language >= application_codes.size()) return language_not_found;
        rsp.push(code(application_codes[language])); return 0;
    });
    s->add_cmd(3, "ConvertLanguageCodeToApplicationLanguage", [](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = request(req, 8)) return rc;
        auto input = req.arg<uint64_t>(0);
        if (input == code("zh-CN")) input = code("zh-Hans");
        if (input == code("zh-TW")) input = code("zh-Hant");
        for (uint8_t i = 0; i < application_codes.size(); ++i)
            if (input == code(application_codes[i])) { rsp.push(i); return 0; }
        return language_not_found;
    });
    return s;
}
}
namespace locale {
uint64_t native_language_code() { return code("en-US"); }
}
void register_locale() {
    const auto process = kernel::current_process();
    add("set", [process] { return settings(process); });
    for (const auto name : {"ns:am2", "ns:ro"}) add(name, [process] {
        auto s = std::make_shared<LocaleService>("IServiceGetterInterface", process);
        s->add_cmd(7989, "GetReadOnlyApplicationControlDataInterface", [process](Request& req, Response& rsp) -> uint32_t {
            if (const auto rc = request(req, 0)) return rc;
            rsp.push_object(application_control(process)); return 0;
        });
        return s;
    });
}
}
