# SPDX-License-Identifier: GPL-3.0-or-later
set(platform_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_platform STATIC
    "${platform_root}/src/mem/host_vm_posix.cpp"
    "${platform_root}/src/mem/subpage.cpp"
    "${platform_root}/src/os/sync.cpp"
    "${platform_root}/src/diag/config.cpp"
    "${platform_root}/src/diag/log.cpp"
    "${platform_root}/src/diag/gauge_registry.cpp")
target_include_directories(nxrt_ps5_platform PUBLIC "${platform_root}/include" "${CMAKE_CURRENT_LIST_DIR}/../recomp/include")
target_compile_definitions(nxrt_ps5_platform PUBLIC NXRECOMP_SUBPAGE_CHECKS=1)
target_compile_features(nxrt_ps5_platform PUBLIC cxx_std_20)
if(PS5)
    target_compile_definitions(nxrt_ps5_platform PUBLIC NXRT_TARGET_PS5=1)
    target_sources(nxrt_ps5_platform PRIVATE "${platform_root}/src/mem/ps5_vm.cpp"
        "${platform_root}/src/mem/guest_region.cpp" "${platform_root}/src/mem/shared_heap.cpp"
        "${platform_root}/src/mem/memory_ps5.cpp")
endif()
find_package(Threads REQUIRED)
target_link_libraries(nxrt_ps5_platform PUBLIC Threads::Threads)
