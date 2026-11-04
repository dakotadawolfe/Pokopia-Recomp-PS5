# SPDX-License-Identifier: GPL-3.0-or-later
# Explicit NVN translation-unit inventory. Alone this target checks compilation;
# nvn_runtime.cmake also links every object when the bundled replay mode is on.
set(nvn_root "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/gfx/nvn")
set(nvn_units
    api_blend_color api_builders api_cmd_state api_cmd_work api_cmdbuf api_depth_poly_ms
    api_device api_memory api_program api_sampler api_texture_builder api_texture api_vertex
    nvn_abi nvn_cmd_handles nvn_enums nvn_format nvn_gpl nvn_gpu_pool nvn_gpu_texture nvn_gpu
    nvn_loader nvn_mask_clear nvn_objects nvn_pipeline nvn_present_pass nvn_present nvn_prewarm
    nvn_register nvn_replay nvn_shaders nvn_spirv_patch nvn_tex_edit nvn_tex_override nvn_tex_patch
    nvn_texdata nvn_texprep nvn_texture_layout nvn_texture_registry nvn_texup)
set(nvn_sources "")
foreach(unit IN LISTS nvn_units)
    list(APPEND nvn_sources "${nvn_root}/${unit}.cpp")
endforeach()
add_library(nxrt_ps5_nvn_objects OBJECT EXCLUDE_FROM_ALL ${nvn_sources})
target_include_directories(nxrt_ps5_nvn_objects PRIVATE "${nvn_root}" "${nvn_root}/.." "${nvn_root}/third_party"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/tiling/include" "${CMAKE_CURRENT_LIST_DIR}/../tools/textures/include")
target_link_libraries(nxrt_ps5_nvn_objects PRIVATE nxrt_ps5_gfx)
target_compile_features(nxrt_ps5_nvn_objects PRIVATE cxx_std_20)
target_compile_options(nxrt_ps5_nvn_objects PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
