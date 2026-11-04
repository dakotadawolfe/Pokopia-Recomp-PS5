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
set(platform_test_sources sync_main.cpp "${root}/runtime/tests/test_sync.cpp"
    "${root}/runtime/tests/test_log_config.cpp" test_host_vm.cpp test_subpage.cpp)
add_executable(ps5_sync_tests ${platform_test_sources})
add_executable(ps5_memory_capabilities memory_capabilities.cpp)
target_include_directories(ps5_sync_tests PRIVATE "${root}/runtime/include" "${root}/runtime/tests")
target_compile_options(ps5_sync_tests PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(ps5_sync_tests PRIVATE nxrt_ps5_core subpage_fixture)
if(PS5)
    target_sources(ps5_sync_tests PRIVATE test_guest_region.cpp)
    target_compile_definitions(ps5_sync_tests PRIVATE NXRT_TEST_PS5=1)
    target_compile_definitions(ps5_memory_capabilities PRIVATE NXRT_TEST_PS5=1)
    add_executable(ps5_shared_heap_tests sync_main.cpp test_shared_heap.cpp)
    target_include_directories(ps5_shared_heap_tests PRIVATE "${root}/runtime/tests")
    target_compile_definitions(ps5_shared_heap_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_SHARED_HEAP=1)
    target_compile_options(ps5_shared_heap_tests PRIVATE -Wall -Wextra -Werror -mcx16)
    target_link_libraries(ps5_shared_heap_tests PRIVATE nxrt_ps5_platform)
    target_link_options(ps5_shared_heap_tests PRIVATE
        "LINKER:--wrap,sceKernelMapDirectMemory" "LINKER:--wrap,mprotect")
    add_executable(ps5_memory_api_tests sync_main.cpp test_memory_ps5.cpp protect_fault.cpp "${root}/runtime/tests/test_memory.cpp")
    target_include_directories(ps5_memory_api_tests PRIVATE "${root}/runtime/tests")
    target_compile_definitions(ps5_memory_api_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_MEMORY_API=1)
    target_compile_options(ps5_memory_api_tests PRIVATE -Wall -Wextra -Werror -mcx16)
    target_link_libraries(ps5_memory_api_tests PRIVATE nxrt_ps5_platform)
    target_link_options(ps5_memory_api_tests PRIVATE "LINKER:--wrap,mprotect")
    add_executable(ps5_loader_tests sync_main.cpp protect_fault.cpp "${root}/runtime/tests/test_loader.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../app/thread_exit.cpp")
    target_include_directories(ps5_loader_tests PRIVATE "${root}/runtime/tests" "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(ps5_loader_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_LOADER=1
        NXRT_TEST_DATA_DIR="/data/pokopia-recomp/tests/loader-data")
    target_compile_options(ps5_loader_tests PRIVATE -Wall -Wextra -Werror -mcx16)
    target_link_libraries(ps5_loader_tests PRIVATE nxrt_ps5_core)
    target_link_options(ps5_loader_tests PRIVATE "LINKER:--wrap,mprotect" "LINKER:--wrap,pthread_create")
endif()

option(PKP_BUILD_NATIVE_APP "Build the runtime suite as a native application" OFF)
set(PKP_NATIVE_APP_ROOT "" CACHE PATH "Pinned ps5-native-app-boilerplate checkout")
if(PKP_BUILD_NATIVE_APP)
    if(NOT PS5 OR NOT EXISTS "${PKP_NATIVE_APP_ROOT}/tooling/native/app_crt.cpp")
        message(FATAL_ERROR "Native app tests require the PS5 SDK and PKP_NATIVE_APP_ROOT")
    endif()
    set(native "${PKP_NATIVE_APP_ROOT}/tooling/native")
    set(sdk "$ENV{PS5_PAYLOAD_SDK}")
    add_executable(ps5_runtime_app ${platform_test_sources} test_guest_region.cpp
        test_app_runtime.cpp "${native}/app_crt.cpp" "${CMAKE_CURRENT_LIST_DIR}/../app/runtime.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../app/thread_exit.cpp" "${CMAKE_CURRENT_LIST_DIR}/../app/lifecycle.cpp"
        test_memory_ps5.cpp protect_fault.cpp "${root}/runtime/tests/test_memory.cpp"
        "${root}/runtime/tests/test_loader.cpp")
    set_property(TARGET ps5_runtime_app PROPERTY SKIP_BUILD_RPATH TRUE)
    target_include_directories(ps5_runtime_app PRIVATE "${root}/runtime/include" "${root}/runtime/tests" "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_options(ps5_runtime_app PRIVATE -Wall -Wextra -Werror -mcx16)
    target_compile_definitions(ps5_runtime_app PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_NATIVE_APP=1
        NXRT_TEST_DATA_DIR="/app0/tests/data")
    target_link_options(ps5_runtime_app PRIVATE -nostartfiles -nodefaultlibs
        "LINKER:-T,${native}/ps5-pie.ld" "LINKER:-T,${CMAKE_CURRENT_LIST_DIR}/../app/unwind.ld"
        "LINKER:--version-script,${CMAKE_CURRENT_LIST_DIR}/../app/exports.map"
        "LINKER:--eh-frame-hdr" "LINKER:--no-dynamic-linker" "LINKER:-e,_start"
        "LINKER:--wrap,mprotect" "LINKER:--wrap,pthread_create")
    target_link_libraries(ps5_runtime_app PRIVATE nxrt_ps5_core subpage_fixture
        "-Wl,--start-group" "${sdk}/target/lib/libc++.a" "${sdk}/target/lib/libc++abi.a"
        "${sdk}/target/lib/libunwind.a" "-Wl,--end-group" "-Wl,--as-needed"
        "${sdk}/target/lib/libSceLibcInternal.so" "${sdk}/target/lib/libkernel.so"
        "${sdk}/target/lib/libc.a" "${sdk}/target/lib/libSceNet.so"
        "${sdk}/target/lib/libSceSystemService.so")
    set_property(TARGET ps5_runtime_app APPEND PROPERTY LINK_DEPENDS
        "${native}/ps5-pie.ld" "${CMAKE_CURRENT_LIST_DIR}/../app/unwind.ld"
        "${CMAKE_CURRENT_LIST_DIR}/../app/exports.map")
endif()

add_executable(ps5_thread_exit_tests sync_main.cpp test_app_runtime.cpp "${CMAKE_CURRENT_LIST_DIR}/../app/thread_exit.cpp")
target_include_directories(ps5_thread_exit_tests PRIVATE "${root}/runtime/tests")
target_link_libraries(ps5_thread_exit_tests PRIVATE nxrt_ps5_platform)
target_link_options(ps5_thread_exit_tests PRIVATE "LINKER:--wrap,pthread_create")
if(PS5)
    target_compile_definitions(ps5_thread_exit_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_THREAD_EXIT=1)
endif()
enable_testing()
add_test(NAME platform COMMAND ps5_sync_tests)
set_tests_properties(platform PROPERTIES TIMEOUT 120)
if(NOT PS5)
    add_test(NAME thread_exit COMMAND ps5_thread_exit_tests)
endif()
