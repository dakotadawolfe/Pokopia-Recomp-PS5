# SPDX-License-Identifier: GPL-3.0-or-later
# Optional diagnostic only. Production game code remains in private local archives.
if(NOT PS5 OR NOT PKP_TEST_SDK OR NOT PKP_GAME_IMAGE_INPUT)
    message(FATAL_ERROR "Game startup tracing requires PS5, PKP_TEST_RTLD, PKP_TEST_SDK and verified game images")
endif()
set(startup_modules main sdk subsdk0 rtld)
include("${CMAKE_CURRENT_LIST_DIR}/exceptions.cmake")
set(startup_eh_tests "${root}/runtime/tests/test_eh.cpp" "${root}/runtime/tests/test_eh_abi3.cpp"
    "${root}/runtime/tests/test_eh_typeinfo.cpp")
set(startup_archives "")
foreach(name IN LISTS startup_modules)
    set(archive "${PKP_SDK_MODULE_BUILD}/libpkp_${name}.a")
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "Missing verified local archive: ${archive}")
    endif()
    list(APPEND startup_archives "${archive}")
    if(NOT name STREQUAL "sdk")
        add_library(startup_${name} STATIC IMPORTED)
        set_target_properties(startup_${name} PROPERTIES IMPORTED_LOCATION "${archive}")
    endif()
endforeach()

set(startup_expectations "${CMAKE_CURRENT_BINARY_DIR}/private-startup")
set(startup_header "${startup_expectations}/private_startup_inputs.h")
option(PKP_BUILD_NATIVE_GAME_STARTUP "Build the full startup trace in native application context" OFF)
set(PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB "512" CACHE STRING "Diagnostic stop threshold before guest heap allocation")
set(PKP_NATIVE_PLATFORM_ARCHIVE "" CACHE FILEPATH "Reviewed PS5 platform archive supplying native directory streams")
set(PKP_NATIVE_PLATFORM_SHA "2d1e0e5eed29ad9a8557e2f33439e7c7a3c930f39b6795d393d3bc79f2df9c30" CACHE STRING "Expected PS5 platform archive SHA-256")
if(PKP_BUILD_NATIVE_GAME_STARTUP)
    if(NOT PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB MATCHES "^[1-9][0-9]*$" OR PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB GREATER 12288)
        message(FATAL_ERROR "Native startup diagnostic heap threshold must be 1..12288 MiB")
    endif()
    if(NOT EXISTS "${PKP_NATIVE_APP_ROOT}/tooling/native/app_crt.cpp")
        message(FATAL_ERROR "Native startup requires the pinned native application toolchain")
    endif()
    set(native "${PKP_NATIVE_APP_ROOT}/tooling/native")
    set(sdk "$ENV{PS5_PAYLOAD_SDK}")
    if(NOT EXISTS "${PKP_NATIVE_PLATFORM_ARCHIVE}")
        message(FATAL_ERROR "Native startup requires the reviewed platform archive used by the graphics app")
    endif()
    file(SHA256 "${PKP_NATIVE_PLATFORM_ARCHIVE}" native_platform_sha)
    if(NOT native_platform_sha STREQUAL PKP_NATIVE_PLATFORM_SHA)
        message(FATAL_ERROR "Native directory platform archive differs from its expected SHA-256")
    endif()
    add_executable(ps5_game_startup_app EXCLUDE_FROM_ALL
        sync_main.cpp test_game_images.cpp game_trace_guards.cpp ${startup_eh_tests} "${startup_header}"
        "${root}/ps5/app/folder_picker_paths.cpp" "${root}/ps5/app/heap.cpp"
        "${root}/ps5/app/startup_checkpoint.cpp"
        "${native}/app_crt.cpp" "${root}/ps5/app/runtime.cpp"
        "${root}/ps5/app/thread_exit.cpp" "${root}/ps5/app/lifecycle.cpp")
    target_include_directories(ps5_game_startup_app PRIVATE "${root}/runtime/tests" "${game_expectations}" "${startup_expectations}")
    target_compile_definitions(ps5_game_startup_app PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_GAME_IMAGES=1
        NXRT_TEST_RTLD=1 NXRT_TEST_SDK=1 NXRT_SDK_STARTUP_TRACE=1 NXRT_GAME_STARTUP_TRACE=1 NXRT_GAME_STARTUP_NATIVE=1 NXRT_GAME_EH=1)
    target_compile_definitions(ps5_game_startup_app PRIVATE PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB=${PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB})
    target_compile_options(ps5_game_startup_app PRIVATE -Wall -Wextra -Werror -mcx16)
    set_property(TARGET ps5_game_startup_app PROPERTY SKIP_BUILD_RPATH TRUE)
    target_link_options(ps5_game_startup_app PRIVATE -nostartfiles -nodefaultlibs
        "LINKER:--build-id=none"
        "LINKER:-T,${native}/ps5-pie.ld" "LINKER:-T,${root}/ps5/app/unwind.ld"
        "LINKER:--version-script,${root}/ps5/app/exports.map" "LINKER:--eh-frame-hdr"
        "LINKER:--no-dynamic-linker" "LINKER:-e,_start")
    foreach(symbol _init_env pthread_create guest_mrs guest_msr guest_trap guest_unimpl call_indirect host_import
                   malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size)
        target_link_options(ps5_game_startup_app PRIVATE "LINKER:--wrap,${symbol}")
    endforeach()
    # Directory streams must share one implementation and DIR representation.
    # The system exports are refused to native titles; the platform uses getdents.
    foreach(symbol opendir fdopendir readdir rewinddir dirfd closedir)
        target_link_options(ps5_game_startup_app PRIVATE "LINKER:--defsym,${symbol}=ps5_${symbol}")
    endforeach()
    target_link_libraries(ps5_game_startup_app PRIVATE nxrt_ps5_svc startup_main test_sdk startup_subsdk0 startup_rtld nxrt_ps5_eh
        "-Wl,--start-group" "${sdk}/target/lib/libc++.a" "${sdk}/target/lib/libc++abi.a"
        "${sdk}/target/lib/libunwind.a" "${PKP_NATIVE_PLATFORM_ARCHIVE}" "-Wl,--end-group" "-Wl,--as-needed"
        "${sdk}/target/lib/libSceLibcInternal.so" "${sdk}/target/lib/libkernel.so"
        "${sdk}/target/lib/libc.a" "${sdk}/target/lib/libSceNet.so" "${sdk}/target/lib/libSceSystemService.so")
    set_property(TARGET ps5_game_startup_app APPEND PROPERTY LINK_DEPENDS
        "${native}/ps5-pie.ld" "${root}/ps5/app/unwind.ld" "${root}/ps5/app/exports.map"
        "${PKP_NATIVE_PLATFORM_ARCHIVE}")
endif()
add_custom_command(OUTPUT "${startup_header}" "${startup_expectations}/startup-inputs.json"
    COMMAND "${NX_PYTHON}" "${root}/ps5/prepare_startup_trace.py"
        "${game_expectations}/game-image-inputs.json" "${PKP_SDK_MODULE_BUILD}" "${startup_expectations}"
    DEPENDS ${startup_archives} "${PKP_SDK_MODULE_BUILD}/module-verification.json"
        "${PKP_SDK_MODULE_BUILD}/CMakeCache.txt" "${game_expectations}/game-image-inputs.json"
        "${root}/ps5/prepare_startup_trace.py" "${root}/ps5/check_modules.py" "${root}/ps5/validate_input.py"
    COMMENT "Verifying full-startup input and archive identities" VERBATIM)
add_executable(ps5_game_startup_trace EXCLUDE_FROM_ALL
    sync_main.cpp test_game_images.cpp game_trace_guards.cpp ${startup_eh_tests} "${root}/ps5/app/thread_exit.cpp" "${startup_header}")
target_include_directories(ps5_game_startup_trace PRIVATE "${root}/runtime/tests" "${game_expectations}" "${startup_expectations}")
target_compile_definitions(ps5_game_startup_trace PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_GAME_IMAGES=1
    NXRT_TEST_RTLD=1 NXRT_TEST_SDK=1 NXRT_SDK_STARTUP_TRACE=1 NXRT_GAME_STARTUP_TRACE=1 NXRT_GAME_EH=1)
target_compile_options(ps5_game_startup_trace PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(ps5_game_startup_trace PRIVATE nxrt_ps5_svc startup_main test_sdk startup_subsdk0 startup_rtld nxrt_ps5_eh)
foreach(symbol pthread_create guest_mrs guest_msr guest_trap guest_unimpl call_indirect host_import)
    target_link_options(ps5_game_startup_trace PRIVATE "LINKER:--wrap,${symbol}")
endforeach()
