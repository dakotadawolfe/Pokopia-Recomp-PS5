// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic commands through the same guest ABI and replay path used by NVN.
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/diag.h"
#include "nvn_abi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" int sceKernelQueryMemoryProtection(void*, void**, void**, uint32_t*);

namespace {
using namespace nxrt;
template<class Pfn> using Fn = nvn::abi::Caller<typename nvn::abi::PfnTraits<Pfn>::Sig>;
struct Api {
#define X(name, pfn) Fn<pfn> name{0};
#include "nvn_api.inc"
#undef X
    size_t load(NVNdevice* device) {
        nvn::abi::Caller<void*(const char*)> bootstrap(hle_address("nvnBootstrapLoader"));
        if (!bootstrap) return 0;
        nvn::abi::Caller<void*(const NVNdevice*, const char*)> get_proc(
            reinterpret_cast<uint64_t>(bootstrap("nvnDeviceGetProcAddress")));
        if (!get_proc) return 0;
        size_t count = 0;
#define X(name, pfn) name = Fn<pfn>(reinterpret_cast<uint64_t>(get_proc(device, #name))); count += name ? 1 : 0;
#include "nvn_api.inc"
#undef X
        return count;
    }
};
struct Session {
    Api api;
    NVNdevice device{};
    NVNqueue queue{};
    NVNmemoryPool pool{};
    NVNcommandBuffer command{};
    NVNwindow window{};
    NVNsync sync{};
    std::array<NVNtexture, 2> textures{};
    std::vector<uint8_t> control = std::vector<uint8_t>(1 << 20);
    std::vector<NVNcommandHandle> handles;
    bool backend = false, dev = false, que = false, mem = false, cmd = false, win = false, syn = false;
    size_t texture_count = 0;
    ~Session() {
        if (que) api.nvnQueueFinish(&queue);
        for (auto handle : handles) api.nvnDeviceFinalizeCommandHandle(&device, handle);
        if (cmd) api.nvnCommandBufferFinalize(&command);
        if (win) api.nvnWindowFinalize(&window);
        if (syn) api.nvnSyncFinalize(&sync);
        for (size_t i = 0; i < texture_count; ++i) api.nvnTextureFinalize(&textures[i]);
        if (mem) api.nvnMemoryPoolFinalize(&pool);
        if (que) api.nvnQueueFinalize(&queue);
        if (dev) api.nvnDeviceFinalize(&device);
        if (backend) nvn::shutdown_backend();
    }
    bool initialize(gfx::Device& gpu, gfx::Renderer& renderer) {
        nvn::BackendDesc desc;
        desc.device = &gpu; desc.renderer = &renderer; desc.keep_aspect = false;
        backend = nvn::install_backend(desc);
        if (!backend) return false;
        const auto loaded = api.load(&device);
        std::printf("NVN_REPLAY_API resolved=%zu implemented=%zu mismatches=%u\n",
            loaded, nvn::implemented_count(), nvn::abi::mismatches());
        if (!loaded || nvn::abi::mismatches()) return false;
        NVNdeviceBuilder db{};
        api.nvnDeviceBuilderSetDefaults(&db); api.nvnDeviceBuilderSetFlags(&db, 0);
        dev = api.nvnDeviceInitialize(&device, &db); if (!dev) return false;
        api.nvnDeviceSetWindowOriginMode(&device, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
        api.nvnDeviceSetDepthMode(&device, NVN_DEPTH_MODE_NEAR_IS_ZERO);
        NVNqueueBuilder qb{};
        api.nvnQueueBuilderSetDevice(&qb, &device); api.nvnQueueBuilderSetDefaults(&qb);
        que = api.nvnQueueInitialize(&queue, &qb); if (!que) return false;
        NVNmemoryPoolBuilder mb{};
        api.nvnMemoryPoolBuilderSetDevice(&mb, &device); api.nvnMemoryPoolBuilderSetDefaults(&mb);
        api.nvnMemoryPoolBuilderSetStorage(&mb, reinterpret_cast<void*>(mem::kHeapBase), 8 << 20);
        api.nvnMemoryPoolBuilderSetFlags(&mb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        mem = api.nvnMemoryPoolInitialize(&pool, &mb); if (!mem) return false;
        cmd = api.nvnCommandBufferInitialize(&command, &device); if (!cmd) return false;
        api.nvnCommandBufferAddCommandMemory(&command, &pool, 1 << 20, 1 << 20);
        api.nvnCommandBufferAddControlMemory(&command, control.data(), control.size());
        syn = api.nvnSyncInitialize(&sync, &device); if (!syn) return false;
        size_t offset = 2 << 20;
        std::array<NVNtexture*, 2> pointers{};
        for (size_t i = 0; i < textures.size(); ++i) {
            NVNtextureBuilder tb{};
            api.nvnTextureBuilderSetDevice(&tb, &device); api.nvnTextureBuilderSetDefaults(&tb);
            api.nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE);
            api.nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
            api.nvnTextureBuilderSetSize2D(&tb, 64, 48);
            api.nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); api.nvnTextureBuilderSetLevels(&tb, 1);
            const auto alignment = api.nvnTextureBuilderGetStorageAlignment(&tb);
            const auto bytes = api.nvnTextureBuilderGetStorageSize(&tb);
            if (!alignment || !bytes) return false;
            offset = (offset + alignment - 1) / alignment * alignment;
            if (offset > (8u << 20) || bytes > (8u << 20) - offset) return false;
            api.nvnTextureBuilderSetStorage(&tb, &pool, static_cast<ptrdiff_t>(offset));
            if (!api.nvnTextureInitialize(&textures[i], &tb)) return false;
            ++texture_count; pointers[i] = &textures[i]; offset += bytes;
        }
        NVNwindowBuilder wb{};
        api.nvnWindowBuilderSetDevice(&wb, &device); api.nvnWindowBuilderSetDefaults(&wb);
        api.nvnWindowBuilderSetTextures(&wb, pointers.size(), pointers.data());
        api.nvnWindowBuilderSetPresentInterval(&wb, 1);
        win = api.nvnWindowInitialize(&window, &wb);
        return win;
    }
    bool submit() {
        const auto handle = api.nvnCommandBufferEndRecording(&command);
        if (!handle) return false;
        handles.push_back(handle); api.nvnQueueSubmitCommands(&queue, 1, &handle);
        api.nvnQueueFinish(&queue); return true;
    }
    bool transfers() {
        constexpr size_t bytes = 0x4000;
        auto* data = reinterpret_cast<uint32_t*>(mem::kHeapBase);
        for (unsigned n = 0; n < 3 * bytes / 4; ++n) data[n] = 0x5a71c300 ^ n;
        api.nvnMemoryPoolFlushMappedRange(&pool, 0, 3 * bytes);
        // GPU writes target a private view; the original stays CPU read-only.
        if (!mem::protect(mem::kHeapBase, 0x10000, mem::Prot::R)) return false;
        const auto address = api.nvnMemoryPoolGetBufferAddress(&pool);
        api.nvnCommandBufferBeginRecording(&command);
        api.nvnCommandBufferClearBuffer(&command, address + bytes, bytes, 0x37c5a19b);
        api.nvnCommandBufferCopyBufferToBuffer(&command, address, address + 2 * bytes, bytes, 0);
        if (!submit()) return false;
        api.nvnMemoryPoolInvalidateMappedRange(&pool, 0, 3 * bytes);
        size_t wrong = 0;
        for (unsigned n = 0; n < bytes / 4; ++n) {
            wrong += data[n] != (0x5a71c300 ^ n);
            wrong += data[bytes / 4 + n] != 0x37c5a19b;
            wrong += data[2 * bytes / 4 + n] != (0x5a71c300 ^ n);
        }
        void* begin = nullptr; void* end = nullptr; uint32_t protection = 0;
        const int rc = sceKernelQueryMemoryProtection(data, &begin, &end, &protection);
        const bool protected_primary = !rc && (protection & 3u) == 1u;
        std::printf("NVN_REPLAY_TRANSFERS wrong_words=%zu/12288 primary_protection=%x imports=%llu\n",
            wrong, protection, static_cast<unsigned long long>(nvn::backend_stats().pools_imported));
        const bool restored = mem::protect(mem::kHeapBase, 0x10000, mem::Prot::RW);
        return !wrong && protected_primary && restored && nvn::backend_stats().pools_imported > 0;
    }
    bool colors(gfx::Renderer& renderer) {
        constexpr float colors[3][4]{{1,0,.25f,1}, {0,1,.5f,1}, {.25f,0,1,1}};
        for (unsigned n = 0; n < 3; ++n) {
            int index = -1; api.nvnWindowAcquireTexture(&window, &sync, &index);
            if (index < 0 || index >= 2) return false;
            NVNtexture* target = &textures[index];
            api.nvnCommandBufferBeginRecording(&command);
            api.nvnCommandBufferSetRenderTargets(&command, 1, &target, nullptr, nullptr, nullptr);
            api.nvnCommandBufferSetViewport(&command, 0, 0, 64, 48);
            api.nvnCommandBufferSetScissor(&command, 0, 0, 64, 48);
            api.nvnCommandBufferClearColor(&command, 0, colors[n], NVN_CLEAR_COLOR_MASK_RGBA);
            if (!submit()) return false;
            std::vector<uint8_t> pixels; uint32_t width = 0, height = 0;
            renderer.capture_next_frame(&pixels, &width, &height);
            api.nvnQueuePresentTexture(&queue, &window, index); api.nvnQueueFinish(&queue);
            if (!width || !height || pixels.size() != size_t(width) * height * 4) return false;
            size_t wrong = 0;
            for (size_t p = 0; p < pixels.size(); p += 4)
                for (size_t c = 0; c < 4; ++c)
                    wrong += std::abs(int(pixels[p+c]) - int(colors[n][c] * 255 + .5f)) > 2;
            std::printf("NVN_REPLAY_COLOR index=%u size=%ux%u wrong_components=%zu/%zu\n", n, width, height, wrong, pixels.size());
            if (wrong) return false;
        }
        const auto stats = nvn::backend_stats();
        return stats.presents == 3 && stats.clears >= 3 && stats.submits >= 4;
    }
};
}

bool pkp_nvn_replay_probe(nxrt::gfx::Device& device, nxrt::gfx::Renderer& renderer) {
    std::puts("NVN_REPLAY_BEGIN game_code=false synthetic_commands=true");
    bool ok = false;
    {
        Session session;
        ok = session.initialize(device, renderer) && session.transfers() && session.colors(renderer);
        std::puts(nxrt::diag_thread_dump().c_str());
    }
    ok &= !nxrt::nvn::backend_installed() && nxrt::log_count(nxrt::LogLevel::Error) == 0;
    std::printf("NVN_REPLAY_RESULT status=%s gameplay_verified=false\n", ok ? "PASS" : "FAIL");
    return ok;
}
