# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
FetchContent_Declare(pkp_astc
    URL https://codeload.github.com/ARM-software/astc-encoder/tar.gz/refs/tags/5.7.0
    URL_HASH SHA256=7c1b28ece59c9c2737e297123a9910c1c548676c7aed87a09d5734e2fa7bdaf0
    EXCLUDE_FROM_ALL)
FetchContent_Declare(pkp_bc7
    URL https://codeload.github.com/richgel999/bc7enc_rdo/tar.gz/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
    SOURCE_SUBDIR none)
foreach(flag CLI SHAREDLIB UNITTEST ISA_NATIVE ISA_SSE41 ISA_SSE2 ISA_NONE ISA_NEON ISA_SVE_128 ISA_SVE_256)
    set(ASTCENC_${flag} OFF CACHE BOOL "" FORCE)
endforeach()
set(ASTCENC_ISA_AVX2 ON CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR ON CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pkp_astc pkp_bc7)
find_program(PKP_ISPC NAMES ispc REQUIRED NO_CMAKE_FIND_ROOT_PATH)
execute_process(COMMAND "${PKP_ISPC}" --version OUTPUT_VARIABLE ispc_stdout ERROR_VARIABLE ispc_stderr RESULT_VARIABLE ispc_rc)
set(ispc_version "${ispc_stdout}${ispc_stderr}")
if(NOT ispc_rc EQUAL 0 OR NOT ispc_version MATCHES "1\\.32\\.0")
    message(FATAL_ERROR "The texture converter requires the pinned host ISPC 1.32.0")
endif()
set(bc7_out "${CMAKE_CURRENT_BINARY_DIR}/bc7e")
file(MAKE_DIRECTORY "${bc7_out}")
# Single-target ELF code uses the x86-64 SysV ABI on both Linux and PS5.
# 64-bit addressing is required for the high guest and native GPU addresses.
add_custom_command(OUTPUT "${bc7_out}/bc7e.o" "${bc7_out}/bc7e_ispc.h"
    COMMAND "${PKP_ISPC}" -O2 "${pkp_bc7_SOURCE_DIR}/bc7e.ispc"
        -o "${bc7_out}/bc7e.o" -h "${bc7_out}/bc7e_ispc.h"
        --target=avx2-i32x8 --arch=x86-64 --target-os=linux --addressing=64 --pic
        --opt=fast-math --opt=disable-assertions --woff
    DEPENDS "${pkp_bc7_SOURCE_DIR}/bc7e.ispc" VERBATIM)
add_library(nxrt_ps5_bc7 STATIC "${bc7_out}/bc7e.o")
set_target_properties(nxrt_ps5_bc7 PROPERTIES LINKER_LANGUAGE CXX)
add_library(nxrt_ps5_textures STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../tools/textures/src/a2b.cpp" "${bc7_out}/bc7e_ispc.h")
target_include_directories(nxrt_ps5_textures PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../tools/textures/include"
    PRIVATE "${bc7_out}")
target_compile_definitions(nxrt_ps5_textures PRIVATE A2B_STATIC_ASTC=1)
target_compile_features(nxrt_ps5_textures PUBLIC cxx_std_20)
target_link_libraries(nxrt_ps5_textures PUBLIC astcdec-avx2-static nxrt_ps5_bc7)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses")
configure_file("${pkp_astc_SOURCE_DIR}/LICENSE.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/astcenc-Apache-2.0.txt" COPYONLY)
configure_file("${pkp_bc7_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/bc7enc-LICENSE.txt" COPYONLY)
