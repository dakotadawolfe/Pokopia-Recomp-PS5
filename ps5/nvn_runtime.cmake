# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/textures.cmake")
add_library(nxrt_ps5_nvn STATIC $<TARGET_OBJECTS:nxrt_ps5_nvn_objects>
    "${CMAKE_CURRENT_LIST_DIR}/gfx/renderer_diagnostics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/gfx/gfx_tuning.cpp")
target_link_libraries(nxrt_ps5_nvn PUBLIC nxrt_ps5_gfx nxrt_ps5_core nxrt_ps5_textures)
target_compile_features(nxrt_ps5_nvn PUBLIC cxx_std_20)
