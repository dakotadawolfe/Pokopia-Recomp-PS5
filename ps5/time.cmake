# SPDX-License-Identifier: GPL-3.0-or-later
add_library(nxrt_ps5_time_support STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services/time_clock.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services/timezone.cpp")
target_include_directories(nxrt_ps5_time_support PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services")
target_compile_features(nxrt_ps5_time_support PUBLIC cxx_std_20)
target_compile_options(nxrt_ps5_time_support PRIVATE -Wall -Wextra -Werror)
