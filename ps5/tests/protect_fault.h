// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>

// Test-only native protection failure; zero disables injection.
extern std::atomic<unsigned> test_fail_protect;
