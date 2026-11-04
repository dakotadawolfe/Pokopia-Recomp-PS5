# SPDX-License-Identifier: GPL-3.0-or-later
# A separate native process: this target must not link or map game modules.
include_guard(GLOBAL)
set(PKP_NATIVE_APP_ROOT "" CACHE PATH "Pinned native application boilerplate checkout")
set(PKP_RADV_SOURCE_ROOT "" CACHE PATH "Pinned PS5 Vulkan checkout with RADV native link recipe")
set(PKP_RADV_SDK_ROOT "" CACHE PATH "SDK containing the pinned RADV platform archive")
set(PKP_MESA_SOURCE_ROOT "" CACHE PATH "Pinned Mesa checkout supplying Vulkan headers")
set(PKP_RADV_ARCHIVE "" CACHE FILEPATH "Namespace-isolated static RADV archive")
set(PKP_RADV_MANIFEST "" CACHE FILEPATH "Isolation manifest for the static RADV archive")
set(PKP_GRAPHICS_RADV_SHA "ce6141d9c0c01afd8b20eee3ac8f031a3054f91dd8e4b2e2ae5ba40fbf100e4e" CACHE STRING "Explicit expected isolated RADV SHA-256")
set(PKP_GRAPHICS_RAW_SHA "7bd32507c7dca420b69356a30f97940758035bdbddce2f99bec2b69010dd1ad6" CACHE STRING "Explicit expected raw RADV SHA-256")
set(PKP_GRAPHICS_PLATFORM_SHA "2d1e0e5eed29ad9a8557e2f33439e7c7a3c930f39b6795d393d3bc79f2df9c30" CACHE STRING "Explicit expected RADV platform SHA-256")
set(PKP_GRAPHICS_NATIVE_REV "dd44bbdc75437332ed22e3ba95126733419ef25a")
set(PKP_GRAPHICS_VULKAN_REV "5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5")
set(PKP_GRAPHICS_MESA_REV "7b59ef27c1b09b9671bc4153c41940c3155c3af2")

function(pkp_graphics_require_revision path revision)
    execute_process(COMMAND git -C "${path}" rev-parse HEAD OUTPUT_VARIABLE actual
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
    if(NOT result EQUAL 0 OR NOT actual STREQUAL revision)
        message(FATAL_ERROR "Graphics dependency ${path} must be at ${revision}")
    endif()
endfunction()

function(pkp_add_graphics_probe target)
    if(NOT PS5 OR NOT PROSPERO OR NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        message(FATAL_ERROR "The graphics diagnostic requires the PS5 toolchain")
    endif()
    foreach(hash PKP_GRAPHICS_RADV_SHA PKP_GRAPHICS_RAW_SHA PKP_GRAPHICS_PLATFORM_SHA)
        string(LENGTH "${${hash}}" hash_length)
        if(NOT hash_length EQUAL 64 OR NOT "${${hash}}" MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "${hash} must be an explicit lowercase SHA-256")
        endif()
    endforeach()
    foreach(variable PKP_NATIVE_APP_ROOT PKP_RADV_SOURCE_ROOT PKP_RADV_SDK_ROOT
                     PKP_MESA_SOURCE_ROOT PKP_RADV_ARCHIVE PKP_RADV_MANIFEST)
        if(NOT EXISTS "${${variable}}")
            message(FATAL_ERROR "Set ${variable} to the documented existing dependency")
        endif()
    endforeach()
    pkp_graphics_require_revision("${PKP_NATIVE_APP_ROOT}" "${PKP_GRAPHICS_NATIVE_REV}")
    pkp_graphics_require_revision("${PKP_RADV_SOURCE_ROOT}" "${PKP_GRAPHICS_VULKAN_REV}")
    pkp_graphics_require_revision("${PKP_MESA_SOURCE_ROOT}" "${PKP_GRAPHICS_MESA_REV}")
    execute_process(COMMAND git -C "${PKP_NATIVE_APP_ROOT}" diff --quiet HEAD -- tooling runtime RESULT_VARIABLE dirty_native)
    execute_process(COMMAND git -C "${PKP_RADV_SOURCE_ROOT}" diff --quiet HEAD -- tools/radv-link.sh vendor/ps5/sdk/stubs RESULT_VARIABLE dirty_vulkan)
    execute_process(COMMAND git -C "${PKP_MESA_SOURCE_ROOT}" diff --quiet HEAD -- include RESULT_VARIABLE dirty_headers)
    if(NOT dirty_native EQUAL 0 OR NOT dirty_vulkan EQUAL 0 OR NOT dirty_headers EQUAL 0)
        message(FATAL_ERROR "Pinned graphics dependency has modified build inputs")
    endif()
    set(platform "${PKP_RADV_SDK_ROOT}/target/lib/libps5platform.a")
    if(NOT EXISTS "${platform}" OR NOT EXISTS "${PKP_MESA_SOURCE_ROOT}/include/vulkan/vulkan.h")
        message(FATAL_ERROR "RADV platform library or pinned Vulkan headers are missing")
    endif()
    file(SHA256 "${PKP_RADV_ARCHIVE}" archive_sha)
    file(SHA256 "${platform}" platform_sha)
    if(NOT archive_sha STREQUAL PKP_GRAPHICS_RADV_SHA OR NOT platform_sha STREQUAL PKP_GRAPHICS_PLATFORM_SHA)
        message(FATAL_ERROR "RADV or platform archive hash differs from the reviewed graphics probe pins")
    endif()
    file(READ "${PKP_RADV_MANIFEST}" manifest)
    string(JSON manifest_sha GET "${manifest}" isolated_sha256)
    string(JSON source_sha GET "${manifest}" source_sha256)
    if(NOT manifest_sha STREQUAL archive_sha OR NOT source_sha STREQUAL PKP_GRAPHICS_RAW_SHA)
        message(FATAL_ERROR "RADV isolation manifest does not match the pinned archive")
    endif()
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(native "${PKP_NATIVE_APP_ROOT}/tooling/native")
    set(sdk "${PS5_PAYLOAD_SDK}")
    # Native CRT imports and C++ headers use the boilerplate SDK. The RADV
    # platform SDK contributes its compatible static runtime/stack layer.
    file(SHA256 "${sdk}/target/lib/libc++.a" native_cxx_sha)
    file(SHA256 "${PKP_RADV_SDK_ROOT}/target/lib/libc++.a" radv_cxx_sha)
    if(NOT native_cxx_sha STREQUAL radv_cxx_sha)
        message(FATAL_ERROR "Native and RADV libc++ archives differ; qualify their ABI before combining them")
    endif()
    foreach(stub Agc AgcDriver)
        if(stub STREQUAL "Agc")
            set(source agc_canary_link_stub.c)
        else()
            set(source agc_driver_canary_link_stub.c)
        endif()
        set(object "${CMAKE_CURRENT_BINARY_DIR}/graphics-stubs/${stub}.o")
        set(library "${CMAKE_CURRENT_BINARY_DIR}/graphics-stubs/libSce${stub}.so")
        add_custom_command(OUTPUT "${library}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/graphics-stubs"
            COMMAND "${CMAKE_C_COMPILER}" -std=c11 -O2 -fPIC -c
                "${PKP_RADV_SOURCE_ROOT}/vendor/ps5/sdk/stubs/${source}" -o "${object}"
            COMMAND "${CMAKE_LINKER}" --shared -soname "libSce${stub}.prx" -o "${library}" "${object}"
            DEPENDS "${PKP_RADV_SOURCE_ROOT}/vendor/ps5/sdk/stubs/${source}" VERBATIM)
        list(APPEND stub_libraries "${library}")
    endforeach()
    add_custom_target(pkp_graphics_stubs DEPENDS ${stub_libraries})
    add_executable(${target} "${root}/ps5/tests/test_graphics_display.cpp"
        "${root}/ps5/tests/graphics_suite/main.cpp" "${native}/app_crt.cpp" "${native}/app_cpp_runtime.cpp"
        "${root}/ps5/app/runtime.cpp" "${root}/ps5/app/thread_exit.cpp" "${root}/ps5/app/lifecycle.cpp")
    add_dependencies(${target} pkp_graphics_stubs)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE PKP_GRAPHICS_STANDALONE=1)
    # Our cleanup wrapper calls RADV's stack wrapper, then the real kernel API.
    # link_graphics_app.sh renames only that wrapper in a private archive copy.
    set_source_files_properties("${root}/ps5/app/thread_exit.cpp" PROPERTIES
        COMPILE_DEFINITIONS "__real_pthread_create=pkp_radv_pthread_create")
    target_include_directories(${target} SYSTEM PRIVATE "${PKP_MESA_SOURCE_ROOT}/include")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -mcx16)
    set_property(TARGET ${target} PROPERTY SKIP_BUILD_RPATH TRUE)
    target_link_options(${target} PRIVATE -nostartfiles -nodefaultlibs
        "LINKER:-T,${native}/ps5-pie.ld" "LINKER:-T,${root}/ps5/app/unwind.ld"
        "LINKER:--version-script,${root}/ps5/app/exports.map" "LINKER:--eh-frame-hdr"
        "LINKER:--no-dynamic-linker" "LINKER:-e,_start")
    target_link_libraries(${target} PRIVATE ${stub_libraries}
        "${sdk}/target/lib/libSceLibcInternal.so" "${sdk}/target/lib/libkernel.so"
        "${sdk}/target/lib/libc.a" "${sdk}/target/lib/libSceNet.so"
        "${sdk}/target/lib/libSceSystemService.so" "${sdk}/target/lib/libSceVideoOut.so"
        "${sdk}/target/lib/libSceSysmodule.so")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${PKP_RADV_ARCHIVE}" "${platform}"
        "${root}/ps5/link_graphics_app.sh" "${PKP_RADV_SOURCE_ROOT}/tools/radv-link.sh"
        "${native}/ps5-pie.ld" "${root}/ps5/app/unwind.ld" "${root}/ps5/app/exports.map")
    set(CMAKE_CXX_LINK_EXECUTABLE
        "bash \"${root}/ps5/link_graphics_app.sh\" \"${PKP_RADV_SOURCE_ROOT}\" \"${PKP_RADV_SDK_ROOT}\" \"${PKP_RADV_ARCHIVE}\" \"${PKP_GRAPHICS_RADV_SHA}\" \"${PKP_GRAPHICS_PLATFORM_SHA}\" <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>"
        PARENT_SCOPE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/graphics-provenance.json"
        "{\"native_revision\":\"${PKP_GRAPHICS_NATIVE_REV}\",\"vulkan_revision\":\"${PKP_GRAPHICS_VULKAN_REV}\",\"mesa_headers_revision\":\"${PKP_GRAPHICS_MESA_REV}\",\"radv_sha256\":\"${archive_sha}\",\"radv_source_sha256\":\"${source_sha}\",\"platform_sha256\":\"${platform_sha}\",\"compiler\":\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\"frames\":300,\"guest_mappings\":false,\"game_code\":false,\"gameplay_verified\":false}\n")
endfunction()
