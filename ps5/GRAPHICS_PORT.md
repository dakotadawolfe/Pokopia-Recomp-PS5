# Native graphics integration audit

Source/artifact inspection and native diagnostics, 2026-11-04. The standalone display app passed on the PS5. A subsequent combined guest-memory/GPU diagnostic is built and packaged but awaits its own console result. Neither diagnostic runs game code. The later audit sections retain the source boundaries for integrating actual game rendering.

## Standalone diagnostic build

`ps5/tests/graphics_suite` builds `ps5_graphics_app`, packaged separately as **Pokopia Graphics Test**, `PPSA98732`. It uses the existing native shell-exit lifecycle without changing the runtime test runner. It presents 300 frames of red/green/blue/yellow quadrants, rotating their positions each second, checks four GPU readback pixels, and checks FIFO pacing against the selected approximately 60 Hz mode. It writes `/data/pokopia-recomp/tests/graphics-display-<time>-<pid>.log` and requests a return to the shell. Unsupported features and API failures are explicit failures. There are finite acquire/fence waits; Vulkan submission, presentation, and device-idle calls have no API timeout, so a driver hang can still require Options -> Close Game. Resources with uncertain GPU completion are retained for process teardown rather than freed while in use.

No guest code or address mappings are linked into this process. This avoids the guest/RADV VA collision for this diagnostic only. The test proves no NVN behavior or gameplay performance.

The entry point is `extern "C" int pkp_graphics_display_probe()` in `tests/test_graphics_display.cpp`. Its dedicated target is configured by `graphics_probe.cmake`; do not add it to the combined runtime suite. `link_graphics_app.sh` adapts the pinned upstream native recipe. The existing C++ thread-exit wrapper calls the RADV stack wrapper, renamed in a private copy of `libps5platform.a`, then the native pthread API. C++ allocation operators come from the native boilerplate and use the RADV heap. Original SDK archives and common app sources remain unchanged.

The exact reviewed raw archive can be isolated without another project's generated output:

```sh
python3 -B ps5/prepare_graphics_radv.py /path/to/raw/libvulkan_radeon.ps5.a /path/to/isolated
bash ps5/build_graphics_app.sh \
  /path/to/ps5-native-app-boilerplate /path/to/PS5_Vulkan \
  /path/to/PS5_Vulkan/.deps/native/ps5-payload-sdk /path/to/PS5_Mesa \
  /path/to/isolated/libvulkan_radeon.ps5.a /path/to/graphics-build
```

The helper verified all 9,161 renamed symbols and reproduced the default isolated SHA exactly from the default raw SHA. Rebuilding the app against that newly generated archive produced the same `eboot.bin`: **19,047,859 bytes**, SHA-256 `be8b0cdc9aeefb04dd82c0b7324149939aff3c1f1d413ea1aa56f080bcdf9462`. Its native ELF SHA-256 is `e8a8934cf4fd0d2308add9e19e7938f79eecd53021319215b5c053cd64307811`. The reviewed host used SDK Clang 21.1.8 and GNU binutils 2.42.

The installed build passed on the console: `work/ps5/graphics-display-1793789355-386.log` records 300/300 frames at 3840x2160, 59940 mHz, four exact readback pixels, and 280 measured frames in 4.695 seconds against 4.671 expected. Cleanup completed, main returned zero, and the user observed colored quadrants followed by Home. This proves that native Vulkan/VideoOut path and its lifecycle; it does not establish NVN, imported guest memory, or game rendering. The original clear-only package remains preserved separately from the next diagnostic.

### Rebuild the dependencies from public source

The raw archive is a source build with the public Eglantine WSI/import adaptations, not an upstream downloadable release. These commands use fresh dependency directories. They require the host build tools used by the pinned recipes: Git, CMake, Ninja, Meson, Python build dependencies, GNU binutils, ccache, rsync, and the LLVM/Clang/libclc/SPIR-V host dependencies for Mesa's `mesa_clc` and `vtn_bindgen2`. The SDK bootstrap scripts verify their downloadable inputs. No proprietary SDK binaries are committed here.

```sh
project_dir="$(pwd)" # this repository
graphics_deps="$project_dir/work/ps5/graphics-deps"
mkdir -p "$graphics_deps"
git clone https://github.com/drakmor/ps5-native-app-boilerplate.git "$graphics_deps/ps5-native-app-boilerplate"
git -C "$graphics_deps/ps5-native-app-boilerplate" checkout --detach dd44bbdc75437332ed22e3ba95126733419ef25a
git clone https://github.com/mihawk-99/PS5_Vulkan.git "$graphics_deps/mihawk-vulkan-review"
git -C "$graphics_deps/mihawk-vulkan-review" checkout --detach 5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5
git clone https://github.com/mihawk-99/PS5_Mesa.git "$graphics_deps/mihawk-mesa-review"
git -C "$graphics_deps/mihawk-mesa-review" checkout --detach 7b59ef27c1b09b9671bc4153c41940c3155c3af2
git clone https://github.com/mihawk-99/PS5_PayloadSDK.git "$graphics_deps/mihawk-sdk-review"
git -C "$graphics_deps/mihawk-sdk-review" checkout --detach b83202be73e930050e00de0f1b4d9ec46c0391bf
git clone https://github.com/dakotadawolfe/EglantinePS5.git "$graphics_deps/EglantinePS5"
git -C "$graphics_deps/EglantinePS5" checkout --detach 44e3f3e88fd6bd634102525a704bb5956589d507
bash "$graphics_deps/ps5-native-app-boilerplate/tools/setup-native-dependencies.sh"
bash "$graphics_deps/ps5-native-app-boilerplate/tools/build-host-tools.sh"
bash "$graphics_deps/ps5-native-app-boilerplate/tools/rebuild-libc.sh"
RADV_BUILD_JOBS=4 bash "$graphics_deps/EglantinePS5/tools/build-radv-dependencies.sh"
```

The final command builds the pinned SDK/platform and raw release RADV, applies publicly tracked `tools/patch-radv-wsi.py`, `tools/radv-videoout-lifecycle.inc`, and `tools/radv-host-memory.inc`, and rebuilds the changed driver. It modifies only those newly provisioned dependencies and their build outputs. Preserve the resulting `PROVENANCE.txt`, `EDEN_WSI_SHA256`, SDK revision marker, and build logs. The observed adapted WSI source SHA is `63dc9fac0ce053687a4ea23f50a9564f472c5cdd9847cd842f47bd13267bbb32`. The [Eglantine patch-source commit](https://github.com/dakotadawolfe/EglantinePS5/commit/44e3f3e88fd6bd634102525a704bb5956589d507) was publicly accessible on 2026-11-04; `git ls-remote https://github.com/dakotadawolfe/EglantinePS5.git refs/heads/main` returned that exact revision.

The raw build includes debug information. Different host tool versions or source/build paths can change archive bytes; no byte-identical arbitrary-root rebuild is claimed. Defaults retain the reviewed hashes. For a deliberate fresh source build, inspect its provenance and explicitly provide its actual hashes instead of disabling verification:

```sh
raw_driver="$graphics_deps/mihawk-vulkan-review/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
driver_sdk="$graphics_deps/mihawk-vulkan-review/.deps/native/ps5-payload-sdk"
raw_sha="$(sha256sum "$raw_driver" | cut -d' ' -f1)"
platform_sha="$(sha256sum "$driver_sdk/target/lib/libps5platform.a" | cut -d' ' -f1)"
python3 -B ps5/prepare_graphics_radv.py "$raw_driver" "$graphics_deps/isolated" \
  --expected-raw-sha256 "$raw_sha" --record-rebuilt-output
isolated_sha="$(sha256sum "$graphics_deps/isolated/libvulkan_radeon.ps5.a" | cut -d' ' -f1)"
bash ps5/build_graphics_app.sh \
  "$graphics_deps/ps5-native-app-boilerplate" "$graphics_deps/mihawk-vulkan-review" \
  "$driver_sdk" "$graphics_deps/mihawk-mesa-review" \
  "$graphics_deps/isolated/libvulkan_radeon.ps5.a" "$project_dir/work/ps5/graphics-build" \
  "$graphics_deps/isolated/manifest.json" \
  -DPKP_GRAPHICS_RAW_SHA="$raw_sha" -DPKP_GRAPHICS_RADV_SHA="$isolated_sha" \
  -DPKP_GRAPHICS_PLATFORM_SHA="$platform_sha"
```

Hash overrides are explicit cache inputs; mismatches still fail configuration and linking. `graphics-provenance.json` records actual hashes/source revisions/compiler, and `graphics-app-manifest.json` records package file hashes with `console_verified=false`. The isolation helper does not attest that an arbitrary input was built from the documented sources. New source-built artifacts require their own console test. The public full-source recipe is documented from the pinned scripts; it was not rerun during this diagnostic implementation.

## Combined guest-memory and GPU diagnostic

`PKP_GRAPHICS_MEMORY_TEST=ON` selects `tests/test_graphics_memory.cpp` in the same dedicated graphics app and retains title `PPSA98732`. The default remains the original clear-only mode. The new mode uses the production `ps5_vm::Reservation` and `DirectMemory` owners to reserve the full heap, alias, stack, and kernel windows at their actual guest addresses. It also maps one guarded host page at each main/SDK image base; it does not load their images. Physical backing is only seven 16 KiB pages. It maps the same guarded heap backing into the primary and alias windows before Vulkan initialization, then imports the middle alias page using `VK_EXT_external_memory_host`.

The original RADV arena overlapped those guest windows. `build_graphics_radv_partition.py` applies a small, checked-in source transformation to the pinned adapted `radv_ps5_platform.c`: the normal arena becomes `[0x4000000000, 0x6000000000)`, or 128 GiB, and ordinary non-32-bit allocations must land exactly on a claimed granule within that arena. An OS fallback outside it is rejected and released. The existing separate 32-bit GPU window remains `[0x200000000, 0x300000000)`. External imports intentionally retain the guest alias address. This guarantee covers ordinary allocations used by this probe; fixed-address, capture/replay, and sparse paths can accept explicit addresses and need a separate ownership audit before enabling them with guest mappings. No general renderer VA-safety claim follows from this test. This is a driver allocation partition, not a change to guest pointers or process/kernel state.

The helper requires the original adapted platform source SHA-256 `8112c2b41a600043d3096a0198c4e3c2d7dcf6b56e8f738f6ce8eb537943725c`, exact patch anchors, the raw archive's explicit expected hash, and its real compile database. It recompiles only `winsys_ps5_radv_ps5_platform.c.o` into its own directory and replaces that one member in a private archive copy. It checks the unchanged member list and unchanged original source/archive hashes. Generated sources, the compile command, replacement-object hash, and archive hashes are saved beside the private archive. No shared SDK, source tree, or original build output is edited.

Build this mode after the public dependency recipe above, retaining its Meson compile database and generated headers:

```sh
partition_dir="$project_dir/work/ps5/graphics-partition"
compile_db="$graphics_deps/mihawk-vulkan-review/.deps/work/radv-build-ps5-release/compile_commands.json"
python3 -B ps5/build_graphics_radv_partition.py "$raw_driver" "$compile_db" "$partition_dir" \
  --expected-raw-sha256 "$raw_sha"
# Inspect partition-manifest.json and the generated source before accepting its hashes.
partition_raw_sha="$(sha256sum "$partition_dir/libvulkan_radeon.ps5.a" | cut -d' ' -f1)"
python3 -B ps5/prepare_graphics_radv.py "$partition_dir/libvulkan_radeon.ps5.a" \
  "$partition_dir/isolated" --expected-raw-sha256 "$partition_raw_sha" --record-rebuilt-output
partition_isolated_sha="$(sha256sum "$partition_dir/isolated/libvulkan_radeon.ps5.a" | cut -d' ' -f1)"
bash ps5/build_graphics_app.sh \
  "$graphics_deps/ps5-native-app-boilerplate" "$graphics_deps/mihawk-vulkan-review" \
  "$driver_sdk" "$graphics_deps/mihawk-mesa-review" \
  "$partition_dir/isolated/libvulkan_radeon.ps5.a" "$project_dir/work/ps5/graphics-memory-build" \
  "$partition_dir/isolated/manifest.json" \
  -DPKP_GRAPHICS_MEMORY_TEST=ON \
  -DPKP_RADV_PARTITION_MANIFEST="$partition_dir/partition-manifest.json" \
  -DPKP_GRAPHICS_RAW_SHA="$partition_raw_sha" -DPKP_GRAPHICS_RADV_SHA="$partition_isolated_sha" \
  -DPKP_GRAPHICS_PLATFORM_SHA="$platform_sha"
```

The local private rebuild produced raw SHA-256 `d1f377cee27b5842eb981805494866c8a7f87d6c44dd8ed73e79f9ad2756f65c` and isolated SHA-256 `423321a55c3daa7bd8bd361473d1f8d9e3a606a9b04a651c34eacd31ad9c2e9a`. The package is `/root/pokopia-recomp-ps5-graphics-memory/app/package/PPSA98732` in WSL `PS5ColdbootLab`; `eboot.bin` is **19,069,699 bytes**, SHA-256 `81ad5acc606e7716dd25f324cc22bb0c13ce6f722338483ef45b5b6d4de8be0d`. Its native ELF SHA-256 is `8006fcf00b9c4a507ebf893959c940d1485ee17a5e2b54c05fb2f11cc8197121`. Compilation with warnings as errors, linking, packaging, and FSELF integrity inspection passed. All package files were subsequently readback-verified at `/data/homebrew/PPSA98732`; installation receipt `work/ps5/graphics-memory-console-install.json` records the previous and new hashes. Native execution of this new mode remains pending.

The console log is `/data/pokopia-recomp/tests/graphics-memory-<time>-<pid>.log`. It checks imported-pointer identity, 16 KiB alignment, queried import capabilities, allocation placement for the staging/display readback buffers, unchanged primary read-only permissions, and GPU read/write permission on the alias. Eight rounds each compare all 4,096 words for CPU writes copied by the GPU and GPU fills read through both CPU views. Every round checks neighboring guard pages and the main/SDK/stack/kernel marker pages. It then runs the established 300-frame display/readback loop while the guest reservations remain alive. Imported Vulkan memory is released before its physical memory owner, and uncertain GPU completion retains mappings for process teardown.

Two cache paths are measured separately. The first uses Vulkan barriers, fence completion, `vkFlushMappedMemoryRanges`, and `vkInvalidateMappedMemoryRanges`. The second additionally uses explicit `CLFLUSHOPT`/fences on only the test's owned mappings. The pinned RADV implementation's flush/invalidate entry points are no-ops (`radv_device_memory.c:392,398`), so successful import alone cannot establish coherence. A nominal mismatch followed by an explicit-cache pass is intentionally reported as overall FAIL with separate counters; that result would identify required cache maintenance before connecting NVN. A pass covers this bounded transfer/alias workload only, not every shader, allocation size, cache direction, sparse operation, or game renderer path.

## Initial presentation path and reference code

The completed standalone test uses a bounded native Vulkan clear/present loop with the existing RADV driver, before adding the NVN renderer. It validated native imports, AGC initialization, the display surface, swapchain acquisition, submission, FIFO pacing, pixel readback, and orderly surface destruction. It does not validate game startup, translated shaders, NVN commands, or gameplay performance.

There is already reference code for this exact path in WSL `PS5ColdbootLab`, under `/root/eglantineps5-build/mihawk-vulkan-review/radv/radv_smoke.c`: display declarations at line 1930, `display_frame` at 1986, `display_swapchain` at 2055, and display-plane creation around 2119. Extract the bounded display case rather than run the entire driver smoke suite. Its readback coordinates assume 3840x2160; use the chosen surface extent instead. Keep the native app's existing exit/lifecycle handling.

The minimal candidate build inputs for that test are:

- Existing Pokopia native CRT, application lifecycle, logger, and packaging path.
- One display-test source using Vulkan headers and direct function pointers from the static ICD's `radv_GetInstanceProcAddr` (or its public ICD alias). No SDL, ImGui, VMA, volk, NVN, shader translator, or texture transcoder is required for a clear.
- RADV archive, linked with `--whole-archive`: the driver's weak dispatch-table references do not pull all implementation objects into an ordinary archive link.
- RADV's compatible `libps5platform.a`, `libc++.a`, `libc++abi.a`, `libunwind.a`, and Clang x86-64 builtins, plus AGC/AGC-driver and native kernel/libc/VideoOut/SystemService imports. Apply the platform's thread/TLS/libc bindings and native import conversion; adding only the Vulkan archive is insufficient.

The concrete reference link recipe is `mihawk-vulkan-review/tools/radv-link.sh:20` and the full native example is `tools/build-radv-title.sh:74`. The latter is evidence to adapt, not a command to run wholesale: it packages its own title and removes its own output directory. Eglantine's `tools/radv-link-eden.sh:4` adapts the recipe to an existing custom heap and NFS wrappers; Pokopia must reconcile its own wrappers instead of copying those Eden-specific adjustments.

A CPU-rendered status screen is even smaller: `ps5-native-app-boilerplate/src/demo_renderer.cpp:384` allocates two buffers, registers them with VideoOut, and flips one. It proves native scanout only. The demo loops forever and is not an appropriate shutdown implementation for this application. Do not open this VideoOut presenter alongside RADV's presenter.

## Reusable surface and artifact

The Windows sibling `../EglantinePS5/headless/vulkan_surface.cpp:10` is a small reusable surface algorithm: enumerate the single GPU/display, enumerate modes, choose the approximately 60 Hz mode, and call `vkCreateDisplayPlaneSurfaceKHR` with plane zero. Replace its Eden window/display types with the `nxrt::platform` interface. Enable `VK_KHR_surface` and `VK_KHR_display`. The driver implements VideoOut behind the surface; `mihawk-mesa-review/src/vulkan/wsi/wsi_common_videoout.c:366` owns the process's VideoOut handle.

Existing artifact, verified by SHA-256 during this audit:

```
/root/eglantineps5-build/EglantinePS5/build/radv-isolated/libvulkan_radeon.ps5.a
ce6141d9c0c01afd8b20eee3ac8f031a3054f91dd8e4b2e2ae5ba40fbf100e4e
```

Its adjacent `manifest.json` identifies the input release archive hash as `7bd32507c7dca420b69356a30f97940758035bdbddce2f99bec2b69010dd1ad6`. Isolation renamed 9,161 private symbols and retained four public ICD entry points. This confirms the local file and namespace transformation, not that the current source checkouts built that binary or that it works inside Pokopia. The Eglantine link inputs are at `../EglantinePS5/headless/vulkan.cmake:94`; its static-loader adaptation is in `tools/prepare-vulkan-port.py:852`.

Observed WSL checkout HEADs, without asserting clean worktrees or a source-to-binary match:

| Reference | HEAD |
| --- | --- |
| `mihawk-mesa-review` | `7b59ef27c1b09b9671bc4153c41940c3155c3af2` |
| `mihawk-vulkan-review` | `5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5` |
| `EglantinePS5` | `44e3f3e88fd6bd634102525a704bb5956589d507` |
| `ps5-native-app-boilerplate` | `dd44bbdc75437332ed22e3ba95126733419ef25a` |

## Address space and imported memory

Resolve the GPU/guest allocation layout before combining real game mappings with this driver. `runtime/include/nxrt/memory.h:15` places main at `0x7100000000`, SDK at `0x7500000000`, heap at `0x7800000000`, and aliases at `0x6000000000`. Local Mesa source `src/amd/vulkan/winsys/ps5/radv_ps5_platform.c:147` assigns RADV a 256 GiB device-memory region beginning at `0x4000000000`, ending at `0x8000000000`. The separate 32-bit GPU window lies at `0x200000000` to `0x300000000`.

This overlap is a layout risk, not proof that the current ordinary allocation path immediately overwrites mappings. At `radv_ps5_platform.c:319`, normal mapping uses a hint and can fall back; replay/sparse paths include fixed mappings (`:521`). A separately configured RADV region below the first guest alias is preferable to rebasing the already lifted game. Verify all reservation ranges and the exact driver artifact before selecting a partition. The same conflict is already recorded in `ps5/CMakeLists.txt:28` and Eglantine's `src/host_memory.cpp:158`.

Host import exists in the inspected driver source: `radv_ps5_winsys.c:385` enables user pointers, `radv_physical_device.c:919` exposes `VK_EXT_external_memory_host`, and `radv_ps5_bo.c:165` imports CPU addresses. Imports must satisfy 16 KiB alignment (`radv_ps5_platform.h:37`) and the GPU address limit. `radv_ps5_platform.c:756` grants GPU access using `sceKernelMprotect` with CPU and GPU read/write permissions.

That protection change must be reconciled with guest page permissions and write tracking. `nvn_gpu_pool.cpp:101` already prefers a separate heap alias through `mem::heap_private_view`, which PS5 provides at `memory_ps5.cpp:243`; verify alias alignment, GPU visibility, and cache coherency on the actual PS5 allocator. Do not enable Windows-style page tracking unchanged: PS5 host protection granularity is 16 KiB, while guest tracking is organized in 4 KiB pages. Imported memory also needs a demonstrated CPU-write/GPU-read and GPU-write/CPU-read coherence test, not just successful allocation.

## From a native clear to the first game frame

The existing game-renderer seam is `gfx::Device::create` then `gfx::Renderer::create`, followed by `nvn::install_backend(BackendDesc)` (`runtime/include/nxrt/nvn_backend.h:25`, implementation `runtime/src/gfx/nvn/nvn_gpu.cpp:980`). A null renderer cannot show frames. The backend starts its render worker and presentation pass at `nvn_gpu.cpp:821`; `nvn_present.cpp:89` acquires a frame and `:134` ends it. `platform.h` exposes Vulkan extension/surface creation separately from the desktop window implementation.

There is no currently buildable PS5 NVN target in `ps5/CMakeLists.txt`. Its minimum source/dependency closure has not been link-verified. The next integration target needs Device/Renderer, VMA implementation, volk with a static ICD loader, Vulkan headers, the NVN API/backend sources, tiling, shader-cache/reflection, and texture conversion/cache access, plus the existing runtime services and diagnostics. Use an explicit source list rather than the desktop recursive glob. Begin with fixed SDR, FIFO, and no desktop overlay or temporal processing; those features still have link references that must be excluded or implemented explicitly.

Concrete blockers:

| Area | Source boundary |
| --- | --- |
| Windows build assumptions | `runtime/CMakeLists.txt:15`; `runtime/cmake/gfx_deps.cmake:58` defines `VK_USE_PLATFORM_WIN32_KHR`; `gfx.cmake:30` recursively includes desktop sources. |
| Loader | `vk_device.cpp:347` calls Streamline or the desktop Vulkan loader. PS5 should initialize volk from the linked ICD instead. |
| Window/screenshots/HDR | `platform.cpp` is SDL; `vk_renderer.cpp:4` imports SDL PNG helpers and `:22` includes Windows display support. |
| Pacing | `frame_limiter.cpp:8` imports Windows waitable timers. FIFO can pace the first presentation test; the final game loop needs a PS5 timing implementation. |
| Memory tracking | `runtime/src/mem/write_watch.cpp:20` uses Windows page APIs. Preserve dirty/coherency semantics when porting, not only function signatures. |
| Shader cache | `nvn_shaders.cpp:1245` uses `GetModuleFileNameA`. Set an explicit console cache path; `:1271` opens `results.tsv` and per-shader SPIR-V files. Missing translations cannot be fixed by recompiling CPU code. |
| Guarded texture workers | `nvn_texprep.cpp:85` and `nvn_tex_edit.cpp:74` use Windows SEH around concurrent memory reads. Exclude optional editing and replace unsafe lifetime assumptions for texture preparation. |
| ASTC conversion | `gfx.cmake:21` requires Windows `.lib` files and decoder DLLs. `tools/textures/src/a2b.cpp:7,75` uses Windows DLL loading. Supply PS5-native static conversion or verified complete offline texture output; merely omitting conversion loses textures. |
| Backend capabilities | `nvn_gpu.cpp:830` requires timeline semaphores; `nvn_gpu_pool.cpp:206` requires host-memory import. Vulkan 1.3, graphics+compute, presentation support, and translated shader features must be checked against the exact driver. |

The decrypted input currently exists at `C:/Games/Pokopia_v2.0.1a/Pokopia` (`base` and `updates`); the old `C:/Games/pokopia` path is absent. This audit did not inspect or generate the private shader/texture output. A visible native clear is a useful integration milestone, but the first real game frame additionally requires startup to reach NVN, valid RomFS reads, translated shader availability, correct texture uploads, and functioning command replay.
