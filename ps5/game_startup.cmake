# SPDX-License-Identifier: GPL-3.0-or-later
# Optional diagnostic only. Production game code remains in private local archives.
if(NOT PS5 OR NOT PKP_TEST_SDK OR NOT PKP_GAME_IMAGE_INPUT)
    message(FATAL_ERROR "Game startup tracing requires PS5, PKP_TEST_RTLD, PKP_TEST_SDK and verified game images")
endif()
set(startup_modules main sdk subsdk0 rtld)
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
add_custom_command(OUTPUT "${startup_header}" "${startup_expectations}/startup-inputs.json"
    COMMAND "${NX_PYTHON}" "${root}/ps5/prepare_startup_trace.py"
        "${game_expectations}/game-image-inputs.json" "${PKP_SDK_MODULE_BUILD}" "${startup_expectations}"
    DEPENDS ${startup_archives} "${PKP_SDK_MODULE_BUILD}/module-verification.json"
        "${PKP_SDK_MODULE_BUILD}/CMakeCache.txt" "${game_expectations}/game-image-inputs.json"
        "${root}/ps5/prepare_startup_trace.py" "${root}/ps5/check_modules.py" "${root}/ps5/validate_input.py"
    COMMENT "Verifying full-startup input and archive identities" VERBATIM)
add_executable(ps5_game_startup_trace EXCLUDE_FROM_ALL
    sync_main.cpp test_game_images.cpp game_trace_guards.cpp "${root}/ps5/app/thread_exit.cpp" "${startup_header}")
target_include_directories(ps5_game_startup_trace PRIVATE "${root}/runtime/tests" "${game_expectations}" "${startup_expectations}")
target_compile_definitions(ps5_game_startup_trace PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_GAME_IMAGES=1
    NXRT_TEST_RTLD=1 NXRT_TEST_SDK=1 NXRT_SDK_STARTUP_TRACE=1 NXRT_GAME_STARTUP_TRACE=1)
target_compile_options(ps5_game_startup_trace PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(ps5_game_startup_trace PRIVATE nxrt_ps5_svc startup_main test_sdk startup_subsdk0 startup_rtld)
foreach(symbol pthread_create guest_mrs guest_msr guest_trap guest_unimpl call_indirect host_import)
    target_link_options(ps5_game_startup_trace PRIVATE "LINKER:--wrap,${symbol}")
endforeach()
