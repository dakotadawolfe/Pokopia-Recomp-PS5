# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(pokopia_fs_tests LANGUAGES CXX)
set(root "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(NOT PS5)
    message(FATAL_ERROR "The fsp-srv integration suite requires the PS5 guest-memory backend")
endif()
include("${root}/ps5/platform.cmake")
include("${root}/ps5/core.cmake")
include("${root}/ps5/kernel.cmake")
include("${root}/ps5/svc.cmake")
if(NOT TARGET nxrt_ps5_romfs)
    include("${root}/ps5/romfs.cmake")
endif()
add_executable(ps5_fs_tests "${root}/ps5/tests/sync_main.cpp" "${root}/ps5/tests/test_fs_ps5.cpp"
    "${root}/ps5/tests/test_save_ps5.cpp" "${root}/runtime/tests/test_svc_fs_volume.cpp"
    "${root}/ps5/app/thread_exit.cpp")
target_include_directories(ps5_fs_tests PRIVATE "${root}/runtime/tests")
target_compile_definitions(ps5_fs_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_FS=1)
target_compile_options(ps5_fs_tests PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(ps5_fs_tests PRIVATE nxrt_ps5_svc nxrt_ps5_romfs)
target_link_options(ps5_fs_tests PRIVATE "LINKER:--wrap,pthread_create"
    "LINKER:--wrap,_fstatfs" "LINKER:--wrap,lstat" "LINKER:--wrap,pwrite")
