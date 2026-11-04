// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "folder_picker.h"
namespace nxrt::app {
class PickerPad {
public:
    ~PickerPad();
    PickerInput poll(uint64_t now_ms);
    const std::string& error() const { return error_; }
private:
    int handle_ = -1;
    bool owned_ = false;
#ifdef NXRT_TARGET_PS5
    bool initialized_ = false;
    uint64_t retry_at_ = 0;
#endif
    std::string error_;
    void close();
};
}
