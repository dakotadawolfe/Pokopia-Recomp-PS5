// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the established display harness into this independent diagnostic.
// The original display target remains a separate, unchanged test mode.
#include "test_graphics_display.cpp"
#include "nxrt/memory.h"
#include "nxrt/ps5_vm.h"
#include <cerrno>

extern "C" int sceKernelQueryMemoryProtection(void*, void**, void**, uint32_t*);

namespace {
constexpr size_t host_page = 0x4000;
constexpr uint32_t word_count = host_page / sizeof(uint32_t);
constexpr uint32_t guard_value = 0x93c5e71bu;
namespace native = nxrt::ps5_vm;
namespace guest = nxrt::mem;
namespace vm = nxrt::host_vm;
void cache_lines(const void* address, size_t bytes);

struct GuestMemory {
    native::Reservation heap = native::Reservation::reserve(guest::kHeapMax, guest::kHeapBase);
    native::Reservation aliases = native::Reservation::reserve(guest::kAliasSize, guest::kAliasBase);
    native::Reservation stack = native::Reservation::reserve(guest::kStackSize, guest::kStackBase);
    native::Reservation kernel = native::Reservation::reserve(guest::kKernelSize, guest::kKernelBase);
    native::Reservation main = native::Reservation::reserve(host_page, guest::kMainBase);
    native::Reservation sdk = native::Reservation::reserve(host_page, guest::kSdkBase);
    native::DirectMemory backing = native::DirectMemory::allocate(7 * host_page);

    uint32_t* primary() { return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(heap.data()) + host_page); }
    uint32_t* alias() { return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(aliases.data()) + host_page); }
    bool initialize() {
        if (vm::page_size() != host_page || !heap || !aliases || !stack || !kernel || !main || !sdk || !backing) {
            std::printf("GUEST_GPU_FAIL operation=guest-reservations errno=%d page_size=%zu\n", errno, vm::page_size());
            return false;
        }
        if (!heap.map(0, backing, 0, 3 * host_page, vm::Access::ReadWrite) ||
            !aliases.map(0, backing, 0, 3 * host_page, vm::Access::ReadWrite) ||
            !main.map(0, backing, 3 * host_page, host_page, vm::Access::ReadWrite) ||
            !sdk.map(0, backing, 4 * host_page, host_page, vm::Access::ReadWrite) ||
            !stack.map(0, backing, 5 * host_page, host_page, vm::Access::ReadWrite) ||
            !kernel.map(0, backing, 6 * host_page, host_page, vm::Access::ReadWrite)) {
            std::printf("GUEST_GPU_FAIL operation=guest-backing-map errno=%d\n", errno);
            return false;
        }
        for (void* pointer : {heap.data(), static_cast<void*>(static_cast<uint8_t*>(heap.data()) + 2 * host_page),
                              main.data(), sdk.data(), stack.data(), kernel.data()}) {
            for (uint32_t i = 0; i < word_count; ++i) static_cast<uint32_t*>(pointer)[i] = guard_value ^ i;
            cache_lines(pointer, host_page);
        }
        for (uint32_t i = 0; i < word_count; ++i) primary()[i] = 0;
        std::printf("GUEST_GPU_MEMORY primary=%p alias=%p bytes=%zu reserved_heap=%zu reserved_alias=%zu\n",
                    static_cast<void*>(primary()), static_cast<void*>(alias()), host_page, heap.size(), aliases.size());
        return heap.protect(host_page, host_page, vm::Access::Read);
    }
    bool guards() {
        for (void* pointer : {heap.data(), static_cast<void*>(static_cast<uint8_t*>(heap.data()) + 2 * host_page),
                              main.data(), sdk.data(), stack.data(), kernel.data()}) {
            cache_lines(pointer, host_page);
            const volatile auto* words = static_cast<uint32_t*>(pointer);
            for (uint32_t i = 0; i < word_count; ++i)
                if (words[i] != (guard_value ^ i)) {
                    std::printf("GUEST_GPU_FAIL operation=guard page=%p word=%u actual=%08x\n", pointer, i, words[i]);
                    return false;
                }
        }
        return true;
    }
    bool protections() {
        void* first = nullptr;
        void* last = nullptr;
        uint32_t primary_flags = 0, alias_flags = 0;
        const int primary_rc = sceKernelQueryMemoryProtection(primary(), &first, &last, &primary_flags);
        const bool primary_covered = reinterpret_cast<uintptr_t>(first) <= reinterpret_cast<uintptr_t>(primary()) &&
            reinterpret_cast<uintptr_t>(last) >= reinterpret_cast<uintptr_t>(primary()) + host_page;
        const int alias_rc = sceKernelQueryMemoryProtection(alias(), &first, &last, &alias_flags);
        const bool alias_covered = reinterpret_cast<uintptr_t>(first) <= reinterpret_cast<uintptr_t>(alias()) &&
            reinterpret_cast<uintptr_t>(last) >= reinterpret_cast<uintptr_t>(alias()) + host_page;
        const bool ok = primary_rc == 0 && alias_rc == 0 && primary_covered && alias_covered &&
            (primary_flags & 3u) == 1u && (alias_flags & 0x33u) == 0x33u;
        std::printf("GUEST_GPU_PROTECTION primary=%08x alias=%08x primary_rc=%08x alias_rc=%08x status=%s\n",
            primary_flags, alias_flags, static_cast<unsigned>(primary_rc), static_cast<unsigned>(alias_rc), ok ? "PASS" : "FAIL");
        return ok;
    }
};

void cache_lines(const void* address, size_t bytes) {
    // Explicit comparison path: identical operation to the pinned driver's
    // cache helper, scoped strictly to this test's owned mappings.
    const auto begin = reinterpret_cast<uintptr_t>(address) & ~uintptr_t{63};
    const auto end = reinterpret_cast<uintptr_t>(address) + bytes;
    __builtin_ia32_mfence();
    for (uintptr_t at = begin; at < end; at += 64)
        __asm__ volatile("clflushopt %0" : "+m"(*reinterpret_cast<volatile char*>(at)));
    __builtin_ia32_mfence();
}

struct GuestGpu {
    Probe& p;
    GuestMemory& guest_memory;
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT{};
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2{};
    PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties{};
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges{};
    PFN_vkCmdCopyBuffer CmdCopyBuffer{};
    PFN_vkCmdFillBuffer CmdFillBuffer{};
    VkBuffer imported{}, stage{};
    VkDeviceMemory imported_memory{}, stage_memory{};
    void* mapped_import = nullptr;
    void* mapped_stage = nullptr;
    uint32_t imported_type_flags = 0;

    bool initialize() {
        if (!p.instance_function(GetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2") ||
            !p.instance_function(GetPhysicalDeviceExternalBufferProperties, "vkGetPhysicalDeviceExternalBufferProperties")) return false;
#define MEMORY_DEVICE(name) name = reinterpret_cast<PFN_vk##name>(p.GetDeviceProcAddr(p.device, "vk" #name)); \
        if (!name) return p.fail("vk" #name, VK_ERROR_EXTENSION_NOT_PRESENT);
        MEMORY_DEVICE(GetMemoryHostPointerPropertiesEXT)
        MEMORY_DEVICE(FlushMappedMemoryRanges)
        MEMORY_DEVICE(CmdCopyBuffer)
        MEMORY_DEVICE(CmdFillBuffer)
#undef MEMORY_DEVICE
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &host;
        GetPhysicalDeviceProperties2(p.physical, &properties);
        if (!host.minImportedHostPointerAlignment || host.minImportedHostPointerAlignment > host_page ||
            reinterpret_cast<uintptr_t>(guest_memory.alias()) % host.minImportedHostPointerAlignment)
            return p.fail("guest-host-import-alignment");
        VkPhysicalDeviceExternalBufferInfo external_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        external_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties external{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        GetPhysicalDeviceExternalBufferProperties(p.physical, &external_info, &external);
        if (!(external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            return p.fail("guest-buffer-host-importable");
        VkExternalMemoryBufferCreateInfo external_buffer{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external_buffer.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.pNext = &external_buffer;
        buffer.size = host_page;
        buffer.usage = external_info.usage;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!p.check(p.CreateBuffer(p.device, &buffer, nullptr, &imported), "create-guest-import-buffer")) return false;
        VkMemoryRequirements requirements{};
        p.GetBufferMemoryRequirements(p.device, imported, &requirements);
        VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (!p.check(GetMemoryHostPointerPropertiesEXT(p.device, external_info.handleType, guest_memory.alias(), &pointer), "guest-host-pointer-properties")) return false;
        VkPhysicalDeviceMemoryProperties memory{};
        p.GetPhysicalDeviceMemoryProperties(p.physical, &memory);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & pointer.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
        if (type == UINT32_MAX || requirements.size > host_page || requirements.alignment > host_page)
            return p.fail("guest-import-memory-requirements");
        imported_type_flags = memory.memoryTypes[type].propertyFlags;
        VkImportMemoryHostPointerInfoEXT import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import_info.handleType = external_info.handleType;
        import_info.pHostPointer = guest_memory.alias();
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &import_info;
        allocation.allocationSize = host_page;
        allocation.memoryTypeIndex = type;
        if (!p.check(p.AllocateMemory(p.device, &allocation, nullptr, &imported_memory), "allocate-guest-import") ||
            !p.check(p.BindBufferMemory(p.device, imported, imported_memory, 0), "bind-guest-import") ||
            !p.check(p.MapMemory(p.device, imported_memory, 0, VK_WHOLE_SIZE, 0, &mapped_import), "map-guest-import")) return false;
        if (mapped_import != guest_memory.alias()) return p.fail("guest-import-identity-mapping");
        buffer.pNext = nullptr;
        if (!p.check(p.CreateBuffer(p.device, &buffer, nullptr, &stage), "create-guest-stage")) return false;
        p.GetBufferMemoryRequirements(p.device, stage, &requirements);
        type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
        if (type == UINT32_MAX) return p.fail("guest-stage-host-visible-memory");
        allocation.pNext = nullptr;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (!p.check(p.AllocateMemory(p.device, &allocation, nullptr, &stage_memory), "allocate-guest-stage") ||
            !p.check(p.BindBufferMemory(p.device, stage, stage_memory, 0), "bind-guest-stage") ||
            !p.check(p.MapMemory(p.device, stage_memory, 0, VK_WHOLE_SIZE, 0, &mapped_stage), "map-guest-stage")) return false;
        const uintptr_t at = reinterpret_cast<uintptr_t>(mapped_stage);
        const uintptr_t display_readback = reinterpret_cast<uintptr_t>(p.pixels);
        if (at < 0x4000000000ull || at + requirements.size > 0x6000000000ull ||
            display_readback < 0x4000000000ull || display_readback >= 0x6000000000ull)
            return p.fail("driver-allocation-outside-private-arena");
        std::printf("GUEST_GPU_IMPORT alias=%p stage=%p display_readback=%p alignment=%llu memory_flags=%08x\n",
            mapped_import, mapped_stage, p.pixels, static_cast<unsigned long long>(host.minImportedHostPointerAlignment), imported_type_flags);
        return guest_memory.protections();
    }

    bool ranges(bool flush, VkDeviceMemory memory) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.size = VK_WHOLE_SIZE;
        return p.check(flush ? FlushMappedMemoryRanges(p.device, 1, &range) : p.InvalidateMappedMemoryRanges(p.device, 1, &range),
                       flush ? "guest-flush-mapped-range" : "guest-invalidate-mapped-range");
    }
    bool submit(bool fill, uint32_t value) {
        if (!p.check(p.ResetCommandBuffer(p.command, 0), "guest-reset-command")) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!p.check(p.BeginCommandBuffer(p.command, &begin), "guest-begin-command")) return false;
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        p.CmdPipelineBarrier(p.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        if (fill) CmdFillBuffer(p.command, imported, 0, host_page, value);
        else {
            const VkBufferCopy copy{0, 0, host_page};
            CmdCopyBuffer(p.command, imported, stage, 1, &copy);
        }
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        p.CmdPipelineBarrier(p.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        if (!p.check(p.EndCommandBuffer(p.command), "guest-end-command") || !p.check(p.ResetFences(p.device, 1, &p.fence), "guest-reset-fence")) return false;
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &p.command;
        p.unsafe_cleanup = true;
        if (!p.check(p.QueueSubmit(p.queue, 1, &submission, p.fence), "guest-submit")) return false;
        p.submitted = true;
        if (!p.check(p.WaitForFences(p.device, 1, &p.fence, VK_TRUE, wait_ns), "guest-fence")) return false;
        p.unsafe_cleanup = false;
        return true;
    }

    bool rounds(bool explicit_cache, bool& data_match) {
        data_match = true;
        for (uint32_t round = 0; round < 8; ++round) {
            if (!guest_memory.heap.protect(host_page, host_page, vm::Access::ReadWrite)) return p.fail("guest-primary-write-permission");
            const uint32_t seed = 0x1b3a0700u + round * 0x00010101u;
            auto* primary = static_cast<volatile uint32_t*>(guest_memory.primary());
            for (uint32_t i = 0; i < word_count; ++i) primary[i] = seed ^ (i * 0x1021u);
            if (explicit_cache) cache_lines(guest_memory.primary(), host_page);
            if (!ranges(true, imported_memory) || !guest_memory.heap.protect(host_page, host_page, vm::Access::Read))
                return p.fail("guest-primary-read-permission");
            if (!submit(false, 0) || !ranges(false, stage_memory)) return false;
            if (explicit_cache) cache_lines(mapped_stage, host_page);
            const auto* stage_words = static_cast<volatile uint32_t*>(mapped_stage);
            uint32_t read_wrong = 0;
            for (uint32_t i = 0; i < word_count; ++i) read_wrong += stage_words[i] != (seed ^ (i * 0x1021u));
            const uint32_t filled = 0xa55a0000u | (round << 8) | (explicit_cache ? 0x71u : 0x39u);
            if (!submit(true, filled) || !ranges(false, imported_memory)) return false;
            if (explicit_cache) {
                cache_lines(guest_memory.primary(), host_page);
                cache_lines(guest_memory.alias(), host_page);
            }
            const auto* alias = static_cast<volatile uint32_t*>(guest_memory.alias());
            uint32_t primary_wrong = 0, alias_wrong = 0;
            for (uint32_t i = 0; i < word_count; ++i) {
                primary_wrong += primary[i] != filled;
                alias_wrong += alias[i] != filled;
            }
            data_match &= read_wrong == 0 && primary_wrong == 0 && alias_wrong == 0;
            std::printf("GUEST_GPU_ROUND cache=%s round=%u cpu_to_gpu_wrong=%u gpu_to_primary_wrong=%u gpu_to_alias_wrong=%u words=%u\n",
                explicit_cache ? "explicit" : "vulkan", round, read_wrong, primary_wrong, alias_wrong, word_count);
            if (!guest_memory.guards() || !guest_memory.protections()) return false;
        }
        return true;
    }
    void cleanup() {
        if (mapped_import) p.UnmapMemory(p.device, imported_memory);
        if (mapped_stage) p.UnmapMemory(p.device, stage_memory);
        if (imported) p.DestroyBuffer(p.device, imported, nullptr);
        if (stage) p.DestroyBuffer(p.device, stage, nullptr);
        if (imported_memory) p.FreeMemory(p.device, imported_memory, nullptr);
        if (stage_memory) p.FreeMemory(p.device, stage_memory, nullptr);
    }
};
}

extern "C" int pkp_graphics_memory_probe() {
    std::puts("GUEST_GPU_BEGIN diagnostic=guest-memory-and-display game_code=false arena=0x4000000000-0x6000000000");
    auto* mappings = new GuestMemory;
    Probe display;
    GuestGpu gpu{display, *mappings};
    bool nominal = false, managed = false;
    bool ok = mappings->initialize() && display.initialize() && gpu.initialize();
    if (ok) ok = gpu.rounds(false, nominal);
    if (ok) ok = gpu.rounds(true, managed);
    uint32_t completed = 0;
    if (ok) {
        const int splash = sceSystemServiceHideSplashScreen();
        std::printf("GUEST_GPU_SPLASH result=%08x\n", static_cast<unsigned>(splash));
        for (; completed < frames; ++completed)
            if (!display.frame(completed)) { ok = false; break; }
        if (ok) ok = display.read_pixels() && mappings->guards() && mappings->protections();
    }
    if (!display.unsafe_cleanup && display.device) gpu.cleanup();
    const bool clean = display.cleanup();
    if (clean) delete mappings;
    else std::puts("GUEST_GPU_CLEANUP retained=guest-mappings reason=unknown-GPU-completion");
    std::printf("GUEST_GPU_RESULT status=%s vulkan_coherent=%s explicit_cache=%s presented=%u/%u cleanup=%s gameplay_verified=false\n",
        ok && nominal && managed && clean ? "PASS" : "FAIL", nominal ? "PASS" : "FAIL", managed ? "PASS" : "FAIL",
        completed, frames, clean ? "complete" : "incomplete");
    return ok && nominal && managed && clean ? 0 : 1;
}
