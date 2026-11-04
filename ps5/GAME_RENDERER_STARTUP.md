# Native game and renderer integration

This opt-in build joins the recompiled game modules to the PS5 Device/Renderer
and NVN backend qualified by [the native renderer batch](NVN_RUNTIME_PORT.md).
It is a bounded startup diagnostic, not a gameplay-qualified application.

The folder picker runs on the same renderer that NVN will present through.
It validates and remembers the user's existing decrypted folder. Game images
are read from that folder and checked against the compiled module identities;
RomFS remains in place. Nothing copies the game assets into the application.

## Graphics handoff

Upstream `runtime/src/core/switch2.cpp` routes Switch 2 graphics initialization
to its host renderer. `ps5/gfx/game_renderer.cpp` adapts that path for PS5. It
registers twelve SDK graphics/display exports and the real NVN bootstrap after
all four modules have been registered and protected. The original import
provider audit runs before these deliberate overrides.

Before enabling the handoff, the adapter requires a live Device, Renderer,
display surface and installed NVN backend. It checks the actual SDK instruction
sequences identifying the initialized flag and driver-pointer slot, including
registers, operand widths and alignment, and requires both targets to be mapped
writable SDK data. Unknown instruction patterns fail before these writes.

The driver, display, layer and window tokens identify the host renderer. They
are owned guest-memory objects with checked creation/use/destruction order;
they are not Nintendo driver implementations. Supported graphics requests go
through the native NVN backend. Unported driver, video, audio and other service
paths are not made successful by this adapter. Presentation stays SDR/FIFO,
with two frame slots and frame generation disabled. Pipeline and texture caches
use `/data/pokopia-recomp/graphics-cache`, outside the selected game folder.

The existing 38 guest exception cases and a host nonlocal-jump case run before
game dispatch. RADV's shader compiler references standard `setjmp`/`longjmp`;
linker aliases route those calls directly to the SDK implementation already
used for guest EH, without adding a C wrapper frame. Packaging rejects an
undefined jump import. This mapping does not provide signal-mask preservation;
these renderer compiler paths do not use it.

The native allocator uses the pinned platform heap at `0x2000000000` with a
16 GiB virtual reservation. RADV remains confined to `[0x4000000000,0x6000000000)`;
guest aliases, heap, stacks and kernel memory retain their separate reservations.
These are virtual address ranges, not extra physical memory. The game keeps its
11 GiB heap-request threshold and pre-allocation check for 1 GiB remaining host
headroom. Combined game/GPU working-set capacity still needs console evidence.

## Build

Prepare the private modules using [GAME_STARTUP_TRACE.md](GAME_STARTUP_TRACE.md),
including the checked exception hooks. Prepare the pinned native SDK and
partitioned RADV using [GRAPHICS_PORT.md](GRAPHICS_PORT.md), and the ISPC host
tool using [NVN_RUNTIME_PORT.md](NVN_RUNTIME_PORT.md). Use a separate build
directory so the proven renderer-only package remains available for rollback.
Append these options to the documented `build_graphics_app.sh` command:

```sh
-DPKP_GRAPHICS_RUNTIME_TEST=ON -DPKP_GRAPHICS_MEMORY_TEST=OFF \
-DPKP_GRAPHICS_NVN_LINK=ON -DPKP_GRAPHICS_GAME_STARTUP=ON \
-DPKP_ISPC:FILEPATH=/absolute/path/to/ispc \
-DPKP_GAME_IMAGE_INPUT:PATH=/absolute/path/to/verified/decrypted/game \
-DPKP_SDK_MODULE_BUILD:PATH=/absolute/path/to/verified/module-build \
-DNX_PYTHON:FILEPATH=/absolute/path/to/python-with-recomp-requirements
```

`graphics-app-manifest.json` records module/archive identities, the native ELF,
exception implementation and packaged file hashes. Private generated inputs,
recompiled binaries and shader output must stay outside Git. The package uses
the existing owned `PPSA98732` title and contains no original game assets.

The instruction decoder's host checks can run without any game files:

```sh
cmake -S ps5/tests/graphics_decode_suite -B work/ps5/graphics-decode -G Ninja
cmake --build work/ps5/graphics-decode
ctest --test-dir work/ps5/graphics-decode --output-on-failure
```

## Startup shader preparation

The native app consumes translated SPIR-V from
`/data/pokopia-recomp/shader_cache/spirv`. These are private generated build
outputs; the selected USB/NAND game tree remains unchanged. Do not overwrite
an existing cache without checking its identity and preserving it first.

The translator uses the pinned Ryubing source revision
`a82350bb774f70fcbd41c9987bf67a3775409963`. Its
[Codeberg source archive](https://codeberg.org/Software_arch/Ryubing/archive/a82350bb774f70fcbd41c9987bf67a3775409963.tar.gz)
has SHA-256 `aba9789bdcf3fda9e90f965636e9e85e9dcc299333fbf5451bcf22ecfd608506`.
Stage its three required projects with
`tools/shaders/scripts/setup_ryujinx.ps1 -Ryubing /path/to/extracted/ryubing`.
The local Linux build used .NET SDK 10.0.200; its official archive was checked
against Microsoft's release-metadata SHA-512 before extraction. These tools
run on the development PC, not the PS5.

```sh
dotnet build tools/shaders/harness/SpirvHarness.csproj -c Release \
  --disable-build-servers --source https://api.nuget.org/v3/index.json
python tools/shaders/ampere/extract_corpus.py \
  --romfs /path/to/verified/update/romfs --main /path/to/verified/update/exefs/main \
  --out work/ps5/shader-corpus --jobs 4 --progress
python ps5/prepare_startup_shader_corpus.py \
  work/ps5/shader-corpus work/ps5/startup-corpus
dotnet tools/shaders/build/bin/Release/net10.0/spirv_harness.dll \
  --batch work/ps5/startup-corpus --out work/ps5/startup-spirv \
  --jobs 8 --target-env vulkan1.3 --spirv-val /path/to/spirv-val
```

The full extraction found 213,329 occurrences and 99,012 unique shader blocks.
The startup selection contains 153 unique blocks referenced by embedded-main
outputs. All 153 translate with status `ok` and pass Vulkan 1.3 SPIR-V
validation: 76 compute, 72 fragment, two geometry and three vertex shaders.
This approximately 18 MiB cache covers built-in startup shaders only. Full
translation was interrupted; its incomplete output was removed and is not
counted as validated coverage. The complete extracted corpus is retained for
the subsequent full translation. No game shader has been exercised on PS5 yet.

Local qualification also includes 23 instruction-decoder checks under Address
and Undefined Behavior Sanitizers, preflight of all fifteen required SDK
exports, full native linking, FSELF inspection, and verified host jump aliases.
The combined candidate is 278,049,217 bytes with SHA-256
`76821ebc9ed67fda10dc215117ab36b6c680c222d05b22b28bf669ec1ce6d3db`.
Its native ELF is 283,817,276 bytes, SHA-256
`60ababa4cc7da79b7cf48c9ec41fdca9f20dc5f839d12595174b1388596c40ba`.
The candidate is installed as `PPSA98732` after ShadowMountPlus confirmed the
owned title was stopped and unmounted. `work/ps5/game-renderer-install.json`
records the previous executable backup and complete SHA-256 readback of both
staged and installed files. The 153 shaders plus cache metadata were separately
readback-verified in the previously absent shader-cache directory;
`game-startup-shaders-install.json` records their hashes and atomic publication.
`game-renderer-metadata.json` records the app provenance update. The selected
game folder and original assets were untouched.

The owned transfer helper (PID 91) was closed, its payload slot removed, and
port 2121 verified closed. `game-renderer-console.json` records cleanup. No app
launch was performed automatically. The combined game/renderer path is awaiting
its first user-driven console run; installation does not establish runtime
success or gameplay.

## Interpreting a run

Reports are `/data/pokopia-recomp/tests/game-native-<time>-<pid>.log`.
The 180-second deadline starts at guest dispatch, after folder selection and
input checks. `GAME_GRAPHICS_BRIDGE` confirms host routing is installed;
`GAME_GRAPHICS_READY` confirms the title called it. `GAME_NVN_PRESENT` records
presentation, while `GAME_RENDER_REPORT` records submissions and draws. Those
counters alone do not establish correct imagery or playable input.

A missing dependency or expired deadline returns Home with status 2 and
`GAME_TRACE_END`. That remains an incomplete run, not a pass. No game frame or
menu has been verified for this combined build. The prior purple-screen run
qualified synthetic NVN replay and texture conversion only.
