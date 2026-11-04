// SPDX-License-Identifier: GPL-3.0-or-later
// The console owns its display. A platform window is the lifetime of one
// KHR_display surface; it is not an SDL window or a second VideoOut presenter.
#include "nxrt/platform.h"
#include "nxrt/log.h"
#include <volk.h>
#include <array>
#include <ctime>

namespace nxrt::platform {
namespace {
bool ready = false, window = false;
VkSurfaceKHR live_surface = VK_NULL_HANDLE;
VkExtent2D pixels{};
uint32_t refresh = 0;
bool fail(const char* operation, VkResult result = VK_ERROR_INITIALIZATION_FAILED) {
    NXLOG_E("platform.ps5", "%s failed (%d)", operation, static_cast<int>(result));
    return false;
}
}
bool init(const InitOptions& opt) {
    if (opt.gamepads) return fail("gamepad initialization is not implemented in this renderer target");
    ready = true;
    return true;
}
bool initialised() { return ready; }
bool create_window(const WindowDesc& desc) {
    if (!ready || window || desc.hidden || desc.width <= 0 || desc.height <= 0)
        return fail("create display window");
    window = true;
    return true;
}
bool has_window() { return window; }
void pixel_size(int* width, int* height) {
    if (width) *width = static_cast<int>(pixels.width);
    if (height) *height = static_cast<int>(pixels.height);
}
float display_refresh_hz() { return static_cast<float>(refresh) / 1000.0f; }
void destroy_window() {
    if (live_surface) { fail("destroy window before Device released its Vulkan surface"); return; }
    window = false;
    pixels = {};
    refresh = 0;
}
void shutdown() { destroy_window(); if (!window) ready = false; }
uint64_t ticks_ns() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}
const char* const* vulkan_instance_extensions(uint32_t* count) {
    static const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    if (count) *count = 2;
    return extensions;
}
bool create_vulkan_surface(void* raw_instance, uint64_t* output) {
    if (!ready || !window || live_surface || !output) return fail("surface ownership");
    *output = 0;
    const auto instance = static_cast<VkInstance>(raw_instance);
    VkPhysicalDevice physical{};
    uint32_t count = 1;
    VkResult rc = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (rc != VK_SUCCESS || count != 1) return fail("single console GPU", rc);
    VkDisplayPropertiesKHR display{};
    count = 1;
    rc = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display);
    if (rc != VK_SUCCESS || count != 1) return fail("single console display", rc);
    std::array<VkDisplayModePropertiesKHR, 16> modes{};
    count = modes.size();
    rc = vkGetDisplayModePropertiesKHR(physical, display.display, &count, modes.data());
    if (rc != VK_SUCCESS) return fail("display modes", rc);
    VkDisplayModeKHR mode{};
    VkExtent2D extent{};
    uint32_t mode_refresh = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& candidate = modes[i];
        if (candidate.parameters.refreshRate >= 59000 && candidate.parameters.refreshRate <= 61000) {
            mode = candidate.displayMode;
            extent = candidate.parameters.visibleRegion;
            mode_refresh = candidate.parameters.refreshRate;
            break;
        }
    }
    if (!mode || extent.width < 4 || extent.height < 4 || extent.width > 8192 || extent.height > 8192)
        return fail("60 Hz display mode");
    VkDisplayPlanePropertiesKHR plane{};
    count = 1;
    rc = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &count, &plane);
    if (rc != VK_SUCCESS || count != 1) return fail("single display plane", rc);
    VkDisplayKHR supported{};
    count = 1;
    rc = vkGetDisplayPlaneSupportedDisplaysKHR(physical, 0, &count, &supported);
    if (rc != VK_SUCCESS || count != 1 || supported != display.display) return fail("display plane match", rc);
    VkDisplayPlaneCapabilitiesKHR caps{};
    rc = vkGetDisplayPlaneCapabilitiesKHR(physical, mode, 0, &caps);
    if (rc != VK_SUCCESS || !(caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR))
        return fail("opaque display plane", rc);
    VkDisplaySurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    info.displayMode = mode;
    info.planeIndex = 0;
    info.planeStackIndex = plane.currentStackIndex;
    info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.globalAlpha = 1.0f;
    info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    info.imageExtent = extent;
    rc = vkCreateDisplayPlaneSurfaceKHR(instance, &info, nullptr, &live_surface);
    if (rc != VK_SUCCESS) return fail("create display plane surface", rc);
    pixels = extent;
    refresh = mode_refresh;
    *output = reinterpret_cast<uint64_t>(live_surface);
    NXLOG_I("platform.ps5", "surface ready %ux%u at %u mHz", pixels.width, pixels.height, refresh);
    return true;
}
void destroy_vulkan_surface(void* raw_instance, uint64_t surface) {
    const auto value = reinterpret_cast<VkSurfaceKHR>(surface);
    if (!value || value != live_surface) { fail("release unowned display surface"); return; }
    vkDestroySurfaceKHR(static_cast<VkInstance>(raw_instance), value, nullptr);
    live_surface = VK_NULL_HANDLE;
}
} // namespace nxrt::platform
