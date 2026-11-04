// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone native display diagnostic. No guest mappings, assets, or shaders.
#if !defined(PKP_GRAPHICS_STANDALONE)
#error "The display probe must run in its own process before any guest mappings"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" PFN_vkVoidFunction VKAPI_CALL radv_GetInstanceProcAddr(VkInstance, const char*);
extern "C" int sceSystemServiceHideSplashScreen();

namespace {
constexpr uint32_t frames = 300, warmup = 20, maximum_images = 8;
constexpr uint64_t wait_ns = 2'000'000'000ull;
#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR) \
    X(GetPhysicalDeviceDisplayPlanePropertiesKHR) X(GetDisplayPlaneSupportedDisplaysKHR) \
    X(GetDisplayPlaneCapabilitiesKHR) X(CreateDisplayPlaneSurfaceKHR) X(DestroySurfaceKHR) \
    X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateSwapchainKHR) \
    X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateImageView) X(DestroyImageView) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(CreateSemaphore) \
    X(DestroySemaphore) X(QueueSubmit) X(CmdPipelineBarrier) X(CmdBeginRendering) \
    X(CmdEndRendering) X(CmdClearAttachments) X(CmdCopyImageToBuffer) X(CreateBuffer) \
    X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges)

struct Probe {
#define DECLARE(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    VkQueue queue{};
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSemaphore acquired{};
    std::array<VkSemaphore, maximum_images> rendered{};
    std::array<VkImage, maximum_images> images{};
    std::array<VkImageView, maximum_images> views{};
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    void* pixels = nullptr;
    VkExtent2D extent{};
    VkFormat format{};
    uint32_t image_count = 0, family = UINT32_MAX, refresh_millihertz = 0;
    bool submitted = false, unsafe_cleanup = false;

    bool fail(const char* operation, VkResult result = VK_ERROR_INITIALIZATION_FAILED) {
        std::printf("GRAPHICS_FAIL operation=%s result=%d\n", operation, static_cast<int>(result));
        return false;
    }
    bool check(VkResult result, const char* operation) {
        return result == VK_SUCCESS || fail(operation, result);
    }
    template<class T> bool instance_function(T& fn, const char* name) {
        fn = reinterpret_cast<T>(radv_GetInstanceProcAddr(instance, name));
        return fn != nullptr || fail(name, VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    bool initialize() {
        auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(radv_GetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(radv_GetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
        auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(radv_GetInstanceProcAddr(nullptr, "vkCreateInstance"));
        if (!enumerate_version || !enumerate_extensions || !create_instance) return fail("static-ICD-entrypoints");
        uint32_t version = 0;
        if (!check(enumerate_version(&version), "instance-version")) return false;
        if (version < VK_API_VERSION_1_3) return fail("Vulkan-1.3-required", VK_ERROR_INCOMPATIBLE_DRIVER);
        std::array<VkExtensionProperties, 128> extensions{};
        uint32_t count = static_cast<uint32_t>(extensions.size());
        if (!check(enumerate_extensions(nullptr, &count, extensions.data()), "instance-extensions")) return false;
        bool surface_extension = false, display_extension = false;
        for (uint32_t i = 0; i < count; ++i) {
            surface_extension |= std::strcmp(extensions[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0;
            display_extension |= std::strcmp(extensions[i].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
        }
        if (!surface_extension || !display_extension) return fail("surface-display-extensions", VK_ERROR_EXTENSION_NOT_PRESENT);
        const char* requested[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Pokopia Graphics Diagnostic";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 2;
        info.ppEnabledExtensionNames = requested;
        if (!check(create_instance(&info, nullptr, &instance), "create-instance")) return false;
#define LOAD_INSTANCE(name) if (!instance_function(name, "vk" #name)) return false;
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        count = 1;
        if (!check(EnumeratePhysicalDevices(instance, &count, &physical), "physical-devices") || count != 1)
            return fail("exactly-one-physical-device");
        VkPhysicalDeviceProperties properties{};
        GetPhysicalDeviceProperties(physical, &properties);
        std::printf("GRAPHICS_DEVICE name=%s api=%u.%u.%u driver=%08x\n", properties.deviceName,
            VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
            VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
        if (properties.apiVersion < VK_API_VERSION_1_3) return fail("device-Vulkan-1.3-required");
        if (!create_surface()) return false;
        count = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        if (!count || count > 32) return fail("queue-family-count");
        std::array<VkQueueFamilyProperties, 32> queues{};
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        for (uint32_t i = 0; i < count; ++i) {
            VkBool32 present = VK_FALSE;
            if (!check(GetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present), "queue-present-support")) return false;
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = i; break; }
        }
        if (family == UINT32_MAX) return fail("graphics-present-queue");
        std::array<VkExtensionProperties, 512> device_extensions{};
        count = static_cast<uint32_t>(device_extensions.size());
        if (!check(EnumerateDeviceExtensionProperties(physical, nullptr, &count, device_extensions.data()), "device-extensions")) return false;
        bool swapchain_extension = false;
        for (uint32_t i = 0; i < count; ++i)
            swapchain_extension |= std::strcmp(device_extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        if (!swapchain_extension) return fail("swapchain-extension", VK_ERROR_EXTENSION_NOT_PRESENT);
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &supported;
        GetPhysicalDeviceFeatures2(physical, &features);
        if (!supported.dynamicRendering) return fail("dynamic-rendering-required", VK_ERROR_FEATURE_NOT_PRESENT);
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled.dynamicRendering = VK_TRUE;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* device_requested[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &enabled;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_requested;
        if (!check(CreateDevice(physical, &device_info, nullptr, &device), "create-device")) return false;
#define LOAD_DEVICE(name) name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); \
        if (!name) return fail("vk" #name, VK_ERROR_EXTENSION_NOT_PRESENT);
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        GetDeviceQueue(device, family, 0, &queue);
        return create_swapchain() && create_commands() && create_readback();
    }

    bool create_surface() {
        VkDisplayPropertiesKHR display{};
        uint32_t count = 1;
        if (!check(GetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display), "display-properties") || count != 1)
            return fail("exactly-one-display");
        std::array<VkDisplayModePropertiesKHR, 16> modes{};
        count = static_cast<uint32_t>(modes.size());
        if (!check(GetDisplayModePropertiesKHR(physical, display.display, &count, modes.data()), "display-modes")) return false;
        VkDisplayModeKHR mode{};
        for (uint32_t i = 0; i < count; ++i) {
            const auto& p = modes[i].parameters;
            if (p.refreshRate >= 59000 && p.refreshRate <= 61000) {
                mode = modes[i].displayMode; extent = p.visibleRegion; refresh_millihertz = p.refreshRate; break;
            }
        }
        if (!mode || extent.width < 4 || extent.height < 4 || extent.width > 8192 || extent.height > 8192)
            return fail("supported-60Hz-display-mode");
        VkDisplayPlanePropertiesKHR plane{};
        count = 1;
        if (!check(GetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &count, &plane), "display-plane-properties") || count != 1)
            return fail("exactly-one-display-plane");
        VkDisplayKHR plane_display{};
        count = 1;
        if (!check(GetDisplayPlaneSupportedDisplaysKHR(physical, 0, &count, &plane_display), "plane-supported-displays") ||
            count != 1 || plane_display != display.display) return fail("plane-display-match");
        VkDisplayPlaneCapabilitiesKHR capabilities{};
        if (!check(GetDisplayPlaneCapabilitiesKHR(physical, mode, 0, &capabilities), "plane-capabilities")) return false;
        if (!(capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)) return fail("opaque-display-plane");
        VkDisplaySurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
        info.displayMode = mode;
        info.planeIndex = 0;
        info.planeStackIndex = plane.currentStackIndex;
        info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.globalAlpha = 1.0f;
        info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        info.imageExtent = extent;
        std::printf("GRAPHICS_DISPLAY width=%u height=%u refresh_millihertz=%u\n", extent.width, extent.height, refresh_millihertz);
        return check(CreateDisplayPlaneSurfaceKHR(instance, &info, nullptr, &surface), "create-display-surface");
    }

    bool create_swapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!check(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities), "surface-capabilities")) return false;
        if (capabilities.currentExtent.width != UINT32_MAX) extent = capabilities.currentExtent;
        constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((capabilities.supportedUsageFlags & usage) != usage ||
            !(capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ||
            !(capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) return fail("surface-usage-alpha-transform");
        if (extent.width < 4 || extent.height < 4 || extent.width > 8192 || extent.height > 8192)
            return fail("surface-extent");
        std::array<VkSurfaceFormatKHR, 32> formats{};
        uint32_t count = static_cast<uint32_t>(formats.size());
        if (!check(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "surface-formats")) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)) {
                format = formats[i].format; break;
            }
        if (format == VK_FORMAT_UNDEFINED) return fail("RGBA8-or-BGRA8-UNORM-surface");
        std::array<VkPresentModeKHR, 16> present_modes{};
        count = static_cast<uint32_t>(present_modes.size());
        if (!check(GetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, present_modes.data()), "present-modes")) return false;
        bool fifo = false;
        for (uint32_t i = 0; i < count; ++i) fifo |= present_modes[i] == VK_PRESENT_MODE_FIFO_KHR;
        if (!fifo) return fail("FIFO-presentation-required");
        uint32_t requested = capabilities.minImageCount < 3 ? 3 : capabilities.minImageCount;
        if (capabilities.maxImageCount && requested > capabilities.maxImageCount) requested = capabilities.maxImageCount;
        if (requested < capabilities.minImageCount || requested > maximum_images) return fail("swapchain-image-count");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = requested;
        info.imageFormat = format;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (!check(CreateSwapchainKHR(device, &info, nullptr, &swapchain), "create-swapchain")) return false;
        image_count = maximum_images;
        if (!check(GetSwapchainImagesKHR(device, swapchain, &image_count, images.data()), "swapchain-images")) return false;
        if (!image_count || image_count > maximum_images) return fail("actual-swapchain-image-count");
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = 0; i < image_count; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!check(CreateImageView(device, &view, nullptr, &views[i]), "create-swapchain-view") ||
                !check(CreateSemaphore(device, &semaphore_info, nullptr, &rendered[i]), "create-present-semaphore")) return false;
        }
        std::printf("GRAPHICS_SWAPCHAIN images=%u format=%u mode=FIFO\n", image_count, static_cast<unsigned>(format));
        return true;
    }

    bool create_commands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        if (!check(CreateCommandPool(device, &pool, nullptr, &command_pool), "create-command-pool")) return false;
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (!check(AllocateCommandBuffers(device, &allocation, &command), "allocate-command-buffer")) return false;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        return check(CreateFence(device, &fence_info, nullptr, &fence), "create-fence") &&
               check(CreateSemaphore(device, &semaphore_info, nullptr, &acquired), "create-acquire-semaphore");
    }

    bool create_readback() {
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = 16;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(CreateBuffer(device, &buffer, nullptr, &readback), "create-readback-buffer")) return false;
        VkMemoryRequirements requirements{};
        GetBufferMemoryRequirements(device, readback, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        GetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i; break;
            }
        if (type == UINT32_MAX) return fail("host-visible-readback-memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        return check(AllocateMemory(device, &allocation, nullptr, &readback_memory), "allocate-readback-memory") &&
            check(BindBufferMemory(device, readback, readback_memory, 0), "bind-readback-memory") &&
            check(MapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &pixels), "map-readback-memory");
    }

    void barrier(VkImage image, VkImageLayout before, VkImageLayout after, VkPipelineStageFlags src_stage,
                 VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
        VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_barrier.srcAccessMask = src_access;
        image_barrier.dstAccessMask = dst_access;
        image_barrier.oldLayout = before;
        image_barrier.newLayout = after;
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = image;
        image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    }
    static constexpr float colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};

    bool frame(uint32_t number) {
        uint32_t index = UINT32_MAX;
        if (!check(AcquireNextImageKHR(device, swapchain, wait_ns, acquired, VK_NULL_HANDLE, &index), "acquire-image")) return false;
        if (index >= image_count) return fail("acquired-image-index");
        if (!check(ResetCommandBuffer(command, 0), "reset-command-buffer")) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(BeginCommandBuffer(command, &begin), "begin-command-buffer")) return false;
        barrier(images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = views[index];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        CmdBeginRendering(command, &rendering);
        const uint32_t half_width = extent.width / 2, half_height = extent.height / 2;
        for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            const uint32_t color = (quadrant + number / 60) % 4;
            std::memcpy(clear.clearValue.color.float32, colors[color], sizeof colors[color]);
            VkClearRect rectangle{};
            rectangle.rect.offset = {static_cast<int32_t>((quadrant & 1) ? half_width : 0),
                                     static_cast<int32_t>((quadrant & 2) ? half_height : 0)};
            rectangle.rect.extent = {(quadrant & 1) ? extent.width - half_width : half_width,
                                     (quadrant & 2) ? extent.height - half_height : half_height};
            rectangle.layerCount = 1;
            CmdClearAttachments(command, 1, &clear, 1, &rectangle);
        }
        CmdEndRendering(command);
        VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (number + 1 == frames) {
            barrier(images[index], layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            std::array<VkBufferImageCopy, 4> copies{};
            for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
                copies[quadrant].bufferOffset = quadrant * 4;
                copies[quadrant].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copies[quadrant].imageOffset = {static_cast<int32_t>((quadrant & 1) ? half_width + (extent.width - half_width) / 2 : half_width / 2),
                                               static_cast<int32_t>((quadrant & 2) ? half_height + (extent.height - half_height) / 2 : half_height / 2), 0};
                copies[quadrant].imageExtent = {1, 1, 1};
            }
            CmdCopyImageToBuffer(command, images[index], layout, readback, 4, copies.data());
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        }
        barrier(images[index], layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        if (!check(EndCommandBuffer(command), "end-command-buffer") || !check(ResetFences(device, 1, &fence), "reset-fence")) return false;
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[index];
        unsafe_cleanup = true;
        if (!check(QueueSubmit(queue, 1, &submit, fence), "submit-frame")) return false;
        submitted = true;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const VkResult presented = QueuePresentKHR(queue, &present);
        if (!check(WaitForFences(device, 1, &fence, VK_TRUE, wait_ns), "frame-fence")) return false;
        unsafe_cleanup = false;
        return check(presented, "present-frame");
    }

    bool read_pixels() {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = readback_memory;
        range.size = VK_WHOLE_SIZE;
        if (!check(InvalidateMappedMemoryRanges(device, 1, &range), "invalidate-readback")) return false;
        const auto* bytes = static_cast<const uint8_t*>(pixels);
        bool matched = true;
        for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
            const uint32_t color = (quadrant + (frames - 1) / 60) % 4;
            std::array<uint8_t, 4> expected{};
            for (uint32_t component = 0; component < 4; ++component) {
                const uint32_t source = format == VK_FORMAT_B8G8R8A8_UNORM && component < 3 ? 2 - component : component;
                expected[component] = static_cast<uint8_t>(colors[color][source] * 255.0f);
                matched &= bytes[quadrant * 4 + component] == expected[component];
            }
            std::printf("GRAPHICS_PIXEL quadrant=%u actual=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n", quadrant,
                bytes[quadrant * 4], bytes[quadrant * 4 + 1], bytes[quadrant * 4 + 2], bytes[quadrant * 4 + 3],
                expected[0], expected[1], expected[2], expected[3]);
        }
        return matched || fail("quadrant-readback-mismatch");
    }

    bool cleanup() {
        if (unsafe_cleanup) {
            std::puts("GRAPHICS_CLEANUP retained=in-flight-resources reason=unknown-GPU-completion");
            return false;
        }
        // Vulkan has no timeout on queue/device idle or presentation calls. A
        // wedged driver still requires shell closure; never free pending work.
        if (submitted && (!DeviceWaitIdle || !check(DeviceWaitIdle(device), "cleanup-device-idle"))) return false;
        if (device) {
            if (pixels && UnmapMemory) UnmapMemory(device, readback_memory);
            if (readback && DestroyBuffer) DestroyBuffer(device, readback, nullptr);
            if (readback_memory && FreeMemory) FreeMemory(device, readback_memory, nullptr);
            for (uint32_t i = 0; i < maximum_images; ++i) {
                if (rendered[i] && DestroySemaphore) DestroySemaphore(device, rendered[i], nullptr);
                if (views[i] && DestroyImageView) DestroyImageView(device, views[i], nullptr);
            }
            if (acquired && DestroySemaphore) DestroySemaphore(device, acquired, nullptr);
            if (fence && DestroyFence) DestroyFence(device, fence, nullptr);
            if (command_pool && DestroyCommandPool) DestroyCommandPool(device, command_pool, nullptr);
            if (swapchain && DestroySwapchainKHR) DestroySwapchainKHR(device, swapchain, nullptr);
            if (DestroyDevice) DestroyDevice(device, nullptr);
        }
        if (surface && DestroySurfaceKHR) DestroySurfaceKHR(instance, surface, nullptr);
        if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
        std::puts("GRAPHICS_CLEANUP complete");
        return true;
    }
};

uint64_t now_ns() {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return static_cast<uint64_t>(time.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(time.tv_nsec);
}
}

extern "C" int pkp_graphics_display_probe() {
    std::puts("GRAPHICS_BEGIN diagnostic=colored-quadrants game_code=false guest_mappings=false");
    Probe probe;
    bool ok = probe.initialize();
    uint32_t completed = 0;
    uint64_t started = 0;
    if (ok) {
        const int splash = sceSystemServiceHideSplashScreen();
        std::printf("GRAPHICS_SPLASH result=%08x\n", static_cast<unsigned>(splash));
        for (; completed < frames; ++completed) {
            if (completed == warmup) started = now_ns();
            if (!probe.frame(completed)) { ok = false; break; }
            if ((completed + 1) % 60 == 0) std::printf("GRAPHICS_PROGRESS presented=%u\n", completed + 1);
        }
        const uint64_t ended = now_ns();
        if (ok) ok = probe.read_pixels();
        if (ok && (!started || ended <= started)) ok = probe.fail("monotonic-clock");
        if (ok) {
            const double elapsed = static_cast<double>(ended - started) / 1e9;
            const double expected = static_cast<double>(frames - warmup) * 1000.0 / probe.refresh_millihertz;
            std::printf("GRAPHICS_PACING frames=%u elapsed_seconds=%.3f expected_seconds=%.3f\n", frames - warmup, elapsed, expected);
            if (elapsed < expected * 0.80 || elapsed > expected * 1.25) ok = probe.fail("FIFO-pacing-outside-25-percent");
        }
    }
    const bool clean = probe.cleanup();
    std::printf("GRAPHICS_RESULT status=%s presented=%u/%u readback=%s cleanup=%s gameplay_verified=false\n",
        ok && clean ? "PASS" : "FAIL", completed, frames, ok ? "verified" : "not-verified", clean ? "complete" : "incomplete");
    return ok && clean ? 0 : 1;
}
