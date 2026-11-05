# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)
include(FetchContent)
enable_language(C)
# Official Xiph release and published SHA-256: https://opus-codec.org/downloads/
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pkp_opus
    URL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
    URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pkp_opus)
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(nxrt_ps5_opus STATIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services/opus_decoder.cpp")
target_include_directories(nxrt_ps5_opus PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/include")
target_compile_features(nxrt_ps5_opus PUBLIC cxx_std_20)
target_compile_options(nxrt_ps5_opus PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nxrt_ps5_opus PUBLIC opus)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/graphics-licenses")
configure_file("${pkp_opus_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/graphics-licenses/opus-BSD-3-Clause.txt" COPYONLY)
