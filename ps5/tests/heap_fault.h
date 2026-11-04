// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
namespace heap_fault {
void fail_allocation(unsigned call);
size_t allocations();
size_t releases();
}
