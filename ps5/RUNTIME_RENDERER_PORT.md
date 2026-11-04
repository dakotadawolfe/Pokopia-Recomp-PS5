# Native runtime renderer integration

2026-11-04. The actual runtime `gfx::Device`, `gfx::Renderer`, VMA allocator, static volk loader, Tegra tiling library, and shader/texture `TestScene` compile and link into a native PS5 app. This replaces the standalone clear harness for the next renderer test. It is not yet NVN command replay or a game frame.

The dedicated `PKP_GRAPHICS_RUNTIME_TEST=ON` mode uses the partitioned driver described in [GRAPHICS_PORT.md](GRAPHICS_PORT.md). Keep `PKP_GRAPHICS_MEMORY_TEST=OFF` and use a separate build directory. The build arguments and explicit archive hashes otherwise match the combined memory probe. `gfx_runtime.cmake` fetches the same pinned Vulkan-Headers/volk SDK 1.4.363.0 and VMA 3.4.0 sources as the desktop project, verifies all three archive hashes, and packages their licenses. No SDL, desktop launcher, ImGui or temporal-upscaler DLL is linked.

```sh
bash ps5/build_graphics_app.sh \
  "$graphics_deps/ps5-native-app-boilerplate" "$graphics_deps/mihawk-vulkan-review" \
  "$driver_sdk" "$graphics_deps/mihawk-mesa-review" \
  "$partition_dir/isolated/libvulkan_radeon.ps5.a" "$project_dir/work/ps5/graphics-runtime-build" \
  "$partition_dir/isolated/manifest.json" \
  -DPKP_GRAPHICS_RUNTIME_TEST=ON \
  -DPKP_RADV_PARTITION_MANIFEST="$partition_dir/partition-manifest.json" \
  -DPKP_GRAPHICS_RAW_SHA="$partition_raw_sha" -DPKP_GRAPHICS_RADV_SHA="$partition_isolated_sha" \
  -DPKP_GRAPHICS_PLATFORM_SHA="$platform_sha"
```

The variables come from the public dependency/partition recipe in GRAPHICS_PORT.md. The local build directory is `/root/pokopia-recomp-ps5-graphics-runtime` in WSL `PS5ColdbootLab`. Packaging reuses title `PPSA98732`; it does not change the native runtime-test title or copy game assets. `graphics-app-manifest.json` is the authoritative hash receipt for the packaged files. Logs use `/data/pokopia-recomp/tests/graphics-runtime-<time>-<pid>.log`.

The initial picker package displayed the UI and cancelled cleanly but exposed a libc `realpath` stub. Its first replacement then exposed native-only `lstat` denial. The [folder picker correction](FOLDER_PICKER.md#native-storage-path-correction) uses opened descriptors and now passes the actual native storage audit, including synthetic selection/save/reload/cleanup. The installed executable is **19,652,449 bytes**, SHA-256 `e9ea79967824e29e6227b5267e5404519ad39407f61d724bb47bdc293462295d`; native ELF SHA-256 `2219778fc2681a7921f5af7dad2e14d6efdfeaf7520be9016780f01805ab8eff`. It was readback-verified, and the audit trigger was removed after the native app exited. The subsequent controller run selected the USB game folder and passed the shader/texture scene, as recorded below.

## What is implemented

- `runtime/src/gfx/vk_device.cpp`: PS5 initializes volk from the namespace-isolated RADV ICD. Windows keeps its Streamline/dynamic loader path. The Windows process-commit discount callback is excluded on PS5; real VMA gauges and accounting remain active.
- `ps5/gfx/platform_ps5.cpp`: owns one real KHR_display surface and chooses the supported approximately 60 Hz mode. It never starts another VideoOut presenter. Surface extent supplies `platform::pixel_size`. Unsupported controller initialization in this minimal platform interface fails explicitly; the folder picker owns its native controller interface separately.
- `runtime/src/gfx/vk_renderer.cpp`: uses the real swapchain, frame slots, synchronization, capture buffer, and submission/presentation code. PS5 currently rejects HDR, temporal/upscale settings, frame generation and latency extras. PNG file encoding/decoding explicitly returns failure; in-memory RGBA capture remains implemented.
- `runtime/src/gfx/frame_limiter.cpp`: retains the schedule/stats algorithm with PS5 monotonic time and interrupt-aware nanosleep. Its performance is not console-qualified yet; FIFO paces the renderer diagnostic.
- `ps5/gfx/frame_trace_ps5.cpp`: optional bounded, serialized trace output using the native monotonic clock. It does not claim Windows thread-stack sampling and rejects that configuration. Synchronous tracing should remain disabled during performance measurements.
- `ps5/tests/test_graphics_runtime.cpp`: retains actual guest heap/alias/stack/kernel reservations, uploads the existing nine-level Tegra texture, creates real vertex/fragment shader pipelines, and renders animated textured quads. Six mip/texture samples are compared against CPU bilinear sampling with a three-byte tolerance; a seventh checks the clear background. A later frame must differ, all 300 scene frames must present, and logged renderer/cleanup errors prevent PASS.

The current native scene uses existing embedded test shaders and generated test textures. It requires no private Pokopia data to render. The launch-time folder picker runs first on the same Device/Renderer; selecting a valid existing folder persists only its location for later game startup. Internal storage and connected USB drives are browsed with the native controller. Invalid/missing folders remain on the picker with an explanation. Cancel returns Home with `CANCELLED`, not a renderer PASS. A valid selection starts the diagnostic scene; it does not make game assets part of the executable or transfer them. `ps5/gfx/picker_draw.cpp` uses the pinned boilerplate's attributed bitmap font plus path punctuation, twelve rows and a two-line status area.

The real renderer contains desktop-era unbounded Vulkan waits. A driver failure can therefore still need Options -> Close Game.

The controller run in `graphics-runtime-1793815568-469.log` selected the existing USB update folder and completed the actual scene. All six sampled texture pixels matched exactly, the background sample passed, both mip levels were covered, and the later frame differed. The renderer presented 300/300 frames with zero errors and returned normally. The user independently reported the animated graphic followed by Home. This proves the native shader/texture scene and picker flow; it does not prove NVN command replay or a game frame.

## Measured NVN closure

`ps5/nvn_compile_audit.cmake` inventories all 40 real NVN translation units explicitly and excludes `nvn_gpu_stub.cpp`. Its optional `nxrt_ps5_nvn_objects` target is outside the default app build. Run the audit with the same native SDK environment:

```sh
cmake --build /path/to/graphics-runtime-build --target nxrt_ps5_nvn_objects -- -k 0 -j 4
```

38 of 40 units compiled locally, including API entry points/builders/commands, memory pools, GPU texture handling, replay, pipeline construction, SPIR-V cache handling, and presentation. Two small portability changes preserve Windows behavior: `api_device.cpp` uses Clang atomic builtins with the original acquire/release ordering on PS5; `nvn_shaders.cpp` uses an explicit console data-directory fallback rather than `GetModuleFileNameA`. Configuration still overrides `/data/pokopia-recomp/shader_cache/spirv`.

The target intentionally still fails on `nvn_texprep.cpp:5` and `nvn_tex_edit.cpp:4`, which include Windows APIs and wrap potentially unmapped guest texture storage in SEH. Removing exception guards would introduce unsafe background reads. The next implementation needs a memory-owner lifetime/snapshot contract, or an explicit synchronous texture path that keeps valid backing alive. These workers were not replaced with successful stubs.

The local `nvn-compile-audit.log` records those failures. `nvn-link-inventory.json` subtracts symbols defined by the 38 objects and the real gfx/platform/volk libraries from their undefined symbols; it is an inventory, not a successful backend link. Remaining dependency groups are concrete:

| Group | Remaining integration |
| --- | --- |
| Memory tracking | `mem::ww` region ownership, dirty tracking, validity, arming and guard faults; PS5 host pages are 16 KiB and guest pages 4 KiB. The pool references belong to optional device-local mirrors; a first UMA backend can explicitly exclude that optimization and retain the real imported-buffer path. The current source has not made that exclusion. |
| Texture workers | `texprep` and `tex_edit` methods from the two blocked units. |
| Texture conversion | Actual `a2b_encode_bc7` implementation/native dependencies or a validated complete offline cache; missing textures cannot be treated as success. |
| Presentation processing | `OutputPass`, FSR1/NIS spatial types, HDR shader transforms, temporal lookup/evaluation references. Required core presentation must stay implemented; unsupported optional paths need explicit exclusion/rejection. |
| Runtime glue | Real HLE registration/dispatch, host callback flags, progress/frame/thread diagnostics, and hitch counters. These should reuse the native runtime, not duplicate kernel/service implementations in graphics. |
| Small portable sources | `gfx_tuning.cpp` and shader-backlog state need inclusion with their real owners. |

No NVN backend executable has linked or run yet. The compiled object count does not establish renderer correctness, shader coverage, game textures, guest lifetime safety, or first-game-frame readiness.
