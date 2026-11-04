// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace nxrt::services {
// Read-only user settings and NS language interfaces. Administrative settings,
// title metadata, installation and network-rights interfaces are not registered.
void register_locale();
namespace locale {
uint64_t native_language_code();
}
}
