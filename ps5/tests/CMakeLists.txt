# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(pokopia_platform_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(root "${CMAKE_CURRENT_LIST_DIR}/../..")
set(NX_PYTHON "python3" CACHE FILEPATH "Host Python with recomp requirements")
find_program(NX_HOST_CLANG NAMES clang-18 REQUIRED NO_CMAKE_FIND_ROOT_PATH)
include("${root}/recomp/cmake/NxRecomp.cmake")
set(fixture_elf "${CMAKE_CURRENT_BINARY_DIR}/subpage_fixture.elf")
add_custom_command(OUTPUT "${fixture_elf}"
    COMMAND "${NX_HOST_CLANG}" --target=aarch64-none-elf -march=armv8.1-a -nostdlib -static -fuse-ld=lld
            "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/subpage_fixture.ld" "${CMAKE_CURRENT_LIST_DIR}/subpage_instructions.S" -o "${fixture_elf}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/subpage_instructions.S" "${CMAKE_CURRENT_LIST_DIR}/subpage_fixture.ld" VERBATIM)
nxrecomp_add_module(subpage_fixture INPUT "${fixture_elf}" NAME subpage_fixture INDEX 0
    BASE 0x7100000000 KIND static_elf SHARDS 1 JOBS 1)
target_compile_definitions(subpage_fixture PUBLIC NXRECOMP_SUBPAGE_CHECKS=1)
include("${CMAKE_CURRENT_LIST_DIR}/../platform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../core.cmake")
add_executable(ps5_sync_tests sync_main.cpp "${root}/runtime/tests/test_sync.cpp"
    "${root}/runtime/tests/test_log_config.cpp" test_host_vm.cpp test_subpage.cpp)
add_executable(ps5_memory_capabilities memory_capabilities.cpp)
target_include_directories(ps5_sync_tests PRIVATE "${root}/runtime/include" "${root}/runtime/tests")
target_compile_options(ps5_sync_tests PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(ps5_sync_tests PRIVATE nxrt_ps5_core subpage_fixture)
if(PS5)
    target_sources(ps5_sync_tests PRIVATE test_guest_region.cpp)
    target_compile_definitions(ps5_sync_tests PRIVATE NXRT_TEST_PS5=1)
    target_compile_definitions(ps5_memory_capabilities PRIVATE NXRT_TEST_PS5=1)
endif()
enable_testing()
add_test(NAME platform COMMAND ps5_sync_tests)
set_tests_properties(platform PROPERTIES TIMEOUT 120)
