// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/services.h"
#include "nxrt/config.h"
#include "nxrt/game.h"

namespace nxrt::services {
TitleProfile title_defaults() {
    TitleProfile p;
    if (const game::Info* g = game::current(); g && g->title_defaults) g->title_defaults(p);
    if (p.supported_languages == 0) p.supported_languages = 1u << static_cast<uint32_t>(Language::AmericanEnglish);
    return p;
}
uint64_t program_id_now() {
    if (config_has("process", "program_id")) return static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    return static_cast<uint64_t>(config_get_int("title", "program_id", static_cast<int64_t>(title_defaults().program_id)));
}
uint64_t system_resource_size_now() {
    const auto title = config_get_int("title", "system_resource_size", static_cast<int64_t>(title_defaults().system_resource_size));
    return static_cast<uint64_t>(config_get_int("process", "system_resource_size", title));
}
} // namespace nxrt::services
