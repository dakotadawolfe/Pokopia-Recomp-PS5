// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "folder_picker.h"
namespace nxrt::app {
std::string picker_directory(const std::string&, const std::vector<StorageRoot>&);
bool picker_package(const std::string&);
}
