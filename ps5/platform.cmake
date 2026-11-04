# SPDX-License-Identifier: GPL-3.0-or-later
set(platform_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_platform STATIC
    "${platform_root}/src/os/sync.cpp"
    "${platform_root}/src/diag/config.cpp"
    "${platform_root}/src/diag/log.cpp"
    "${platform_root}/src/diag/gauge_registry.cpp")
target_include_directories(nxrt_ps5_platform PUBLIC "${platform_root}/include")
target_compile_features(nxrt_ps5_platform PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(nxrt_ps5_platform PUBLIC Threads::Threads)
