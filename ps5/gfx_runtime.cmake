# SPDX-License-Identifier: GPL-3.0-or-later
# Explicit real Device/Renderer closure. This excludes desktop launchers, SDL,
# overlays and temporal upscalers; none are replaced with successful stubs.
include_guard(GLOBAL)
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
FetchContent_Declare(pkp_vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
    SOURCE_SUBDIR none)
FetchContent_Declare(pkp_volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    URL_HASH SHA256=889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d
    SOURCE_SUBDIR none)
FetchContent_Declare(pkp_vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip
    URL_HASH SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(pkp_vulkan_headers pkp_volk pkp_vma)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses")
configure_file("${pkp_volk_SOURCE_DIR}/LICENSE.md" "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/volk-MIT.txt" COPYONLY)
configure_file("${pkp_vma_SOURCE_DIR}/LICENSE.txt" "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/VMA-MIT.txt" COPYONLY)
configure_file("${pkp_vulkan_headers_SOURCE_DIR}/LICENSE.md" "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/Vulkan-Headers.txt" COPYONLY)
configure_file("${pkp_vulkan_headers_SOURCE_DIR}/LICENSES/Apache-2.0.txt" "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/Vulkan-Headers-Apache-2.0.txt" COPYONLY)
configure_file("${pkp_vulkan_headers_SOURCE_DIR}/LICENSES/MIT.txt" "${CMAKE_CURRENT_BINARY_DIR}/graphics-licenses/Vulkan-Headers-MIT.txt" COPYONLY)
add_library(nxrt_ps5_volk STATIC "${pkp_volk_SOURCE_DIR}/volk.c")
target_include_directories(nxrt_ps5_volk SYSTEM PUBLIC
    "${pkp_volk_SOURCE_DIR}" "${pkp_vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(nxrt_ps5_volk PUBLIC VK_NO_PROTOTYPES PRIVATE VOLK_NO_DYNAMIC_LOADER)
set(graphics_root "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/gfx")
set(tiling_root "${CMAKE_CURRENT_LIST_DIR}/../tools/tiling")
add_library(nxrt_ps5_gfx STATIC
    "${graphics_root}/vk_device.cpp" "${graphics_root}/vk_renderer.cpp" "${graphics_root}/vma_impl.cpp"
    "${graphics_root}/frame_limiter.cpp" "${graphics_root}/scene.cpp"
    "${graphics_root}/extras/extras_state.cpp" "${graphics_root}/extras/hdr_math.cpp"
    "${graphics_root}/extras/hdr_capture.cpp" "${graphics_root}/extras/lowlat.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gfx/platform_ps5.cpp" "${CMAKE_CURRENT_LIST_DIR}/gfx/frame_trace_ps5.cpp"
    "${tiling_root}/src/tegra_tiling.cpp")
target_include_directories(nxrt_ps5_gfx PRIVATE "${graphics_root}" "${tiling_root}/include")
target_include_directories(nxrt_ps5_gfx SYSTEM PRIVATE "${pkp_vma_SOURCE_DIR}/include")
target_compile_definitions(nxrt_ps5_gfx PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_compile_features(nxrt_ps5_gfx PUBLIC cxx_std_20)
target_compile_options(nxrt_ps5_gfx PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(nxrt_ps5_gfx PUBLIC nxrt_ps5_platform nxrt_ps5_volk)
include("${CMAKE_CURRENT_LIST_DIR}/nvn_compile_audit.cmake")
