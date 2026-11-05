# SPDX-License-Identifier: GPL-3.0-or-later
# Included only by the graphics app's explicit private-game integration mode.
# Leave room in the native direct-memory pool for the renderer, translated
# textures, shader compilation and host allocations after the guest heap grows.
set(PKP_GRAPHICS_GUEST_MEMORY_MIB 9216)
set(PKP_GAME_IMAGE_INPUT "" CACHE PATH "Existing decrypted game dump")
set(PKP_SDK_MODULE_BUILD "" CACHE PATH "Verified compiled game modules")
set(NX_PYTHON "python3" CACHE FILEPATH "Host Python with recomp requirements")
foreach(required PKP_GAME_IMAGE_INPUT PKP_SDK_MODULE_BUILD)
    if(NOT IS_DIRECTORY "${${required}}")
        message(FATAL_ERROR "Set ${required} to the existing verified private input")
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/kernel.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/svc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/exceptions.cmake")
set(game_expectations "${CMAKE_CURRENT_BINARY_DIR}/private-game-images")
set(startup_expectations "${CMAKE_CURRENT_BINARY_DIR}/private-startup")
execute_process(COMMAND "${NX_PYTHON}" "${root}/ps5/prepare_game_image_test.py"
    "${PKP_GAME_IMAGE_INPUT}" "${game_expectations}" RESULT_VARIABLE input_rc)
if(NOT input_rc EQUAL 0)
    message(FATAL_ERROR "Game renderer input verification failed")
endif()
file(READ "${game_expectations}/game-image-inputs.json" game_manifest)
foreach(index RANGE 0 3)
    string(JSON source GET "${game_manifest}" modules ${index} source)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endforeach()
set(archives "")
foreach(name main sdk subsdk0 rtld)
    set(archive "${PKP_SDK_MODULE_BUILD}/libpkp_${name}.a")
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "Missing game module: ${archive}")
    endif()
    list(APPEND archives "${archive}")
endforeach()
add_custom_command(OUTPUT "${startup_expectations}/private_startup_inputs.h" "${startup_expectations}/startup-inputs.json"
    COMMAND "${NX_PYTHON}" "${root}/ps5/prepare_startup_trace.py"
        "${game_expectations}/game-image-inputs.json" "${PKP_SDK_MODULE_BUILD}" "${startup_expectations}"
    DEPENDS ${archives} "${PKP_SDK_MODULE_BUILD}/module-verification.json"
        "${PKP_SDK_MODULE_BUILD}/CMakeCache.txt" "${game_expectations}/game-image-inputs.json"
        "${root}/ps5/prepare_startup_trace.py" "${root}/ps5/check_modules.py" "${root}/ps5/validate_input.py"
    COMMENT "Verifying private game and renderer integration inputs" VERBATIM)
target_sources(${target} PRIVATE "${startup_expectations}/private_startup_inputs.h"
    "${root}/ps5/tests/test_native_save_storage.cpp"
    "${root}/ps5/tests/game_trace_guards.cpp" "${root}/ps5/gfx/game_renderer.cpp"
    "${root}/ps5/tests/test_host_jumps.cpp"
    "${root}/ps5/app/startup_checkpoint.cpp"
    "${root}/ps5/app/game_input.cpp"
    "${root}/runtime/tests/test_eh.cpp" "${root}/runtime/tests/test_eh_abi3.cpp" "${root}/runtime/tests/test_eh_typeinfo.cpp")
# Import only the platform statistics declaration, without placing another
# SDK's C headers before the compiler's own C++/sysroot headers.
configure_file("${PKP_RADV_SDK_ROOT}/target/include/ps5platform/heap.h"
    "${CMAKE_CURRENT_BINARY_DIR}/platform-headers/ps5platform/heap.h" COPYONLY)
target_include_directories(${target} PRIVATE "${root}/runtime/tests" "${game_expectations}" "${startup_expectations}"
    "${CMAKE_CURRENT_BINARY_DIR}/platform-headers")
target_compile_definitions(${target} PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_GAME_IMAGES=1 NXRT_TEST_RTLD=1
    NXRT_TEST_SDK=1 NXRT_SDK_STARTUP_TRACE=1 NXRT_GAME_STARTUP_TRACE=1 NXRT_GAME_STARTUP_NATIVE=1
    NXRT_GAME_RENDER_NATIVE=1 NXRT_GAME_EH=1 PKP_NATIVE_STARTUP_HEAP_LIMIT_MIB=11264
    PKP_NATIVE_GAME_MEMORY_MIB=${PKP_GRAPHICS_GUEST_MEMORY_MIB})
target_link_libraries(${target} PRIVATE nxrt_ps5_svc nxrt_ps5_eh ${archives})
foreach(symbol _init_env guest_mrs guest_msr guest_trap guest_unimpl call_indirect host_import)
    target_link_options(${target} PRIVATE "LINKER:--wrap,${symbol}")
endforeach()
# RADV's shader compiler also uses host nonlocal jumps. Route its error paths
# to the same real SDK implementation as guest EH, without another C stack frame.
target_link_options(${target} PRIVATE "LINKER:--wrap,setjmp" "LINKER:--wrap,longjmp"
    "LINKER:--defsym,__wrap_setjmp=nxrt_ps5_setjmp" "LINKER:--defsym,__wrap_longjmp=nxrt_ps5_longjmp")
