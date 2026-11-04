# Native NVN renderer batch

2026-11-04. All 40 NVN translation units now compile and link into the native
graphics app, including the real texture workers, static ASTC/BC7 conversion,
memory tracking, HLE dispatch and renderer diagnostics. The link uses
`WHOLE_ARCHIVE`, so unused API objects cannot hide unresolved dependencies.
No successful backend substitutes or game assets are included.

The bundled native run is **not console-verified yet**. It does not execute
Pokopia code, load its shaders or show its menu. The previously verified native
shader scene and the separate game startup diagnostic remain distinct results.

## One bundled launch

Enable `PKP_GRAPHICS_RUNTIME_TEST=ON` and `PKP_GRAPHICS_NVN_LINK=ON`, with
`PKP_GRAPHICS_MEMORY_TEST=OFF`. After the existing folder picker, the app runs:

1. The established 300-frame shader/texture animation and seven readback samples.
2. NVN bootstrap/procedure resolution through the guest calling convention,
   device/queue/pool creation, command recording and Vulkan replay.
3. GPU buffer fill and copy, comparing 12,288 words and checking that the
   primary guest mapping remains read-only while the separate import is used.
4. Three NVN render-target clears, window acquisition, presentation and full
   RGBA frame readback through the actual renderer.
5. ASTC decoding for all 14 footprints in linear/sRGB formats, BC7 qualities
   0–6, thread determinism, alpha, size/error and passthrough cases. An
   independent BC7 reference decoder checks the encoded output.

Each stage writes its own result to `graphics-runtime-<time>-<pid>.log`. A return
to Home is not itself a pass. Failed initialization, mismatched data or logged
renderer errors prevent PASS. Unbounded native driver waits remain a limitation;
this is a development diagnostic, not a release-quality game launcher.

## Port changes and limits

- `mem::copy_readable` validates the complete source and copies it while holding
  the memory owner's mutation lock. Texture workers retain owned snapshots
  instead of relying on Windows SEH around potentially unmapped storage. Sparse
  segments retain their pool references and use the same copy contract. The
  lock prevents unmap/protection races, not concurrent guest writes; existing
  hash/generation checks still decide whether prepared data is current.
- `heap_import_view` refuses holes and partial host pages. PS5 imports must use
  a retained private view; they never fall back to the primary mapping. The
  pinned driver grants GPU permissions to that private view. Vulkan memory is
  freed before the view is released. Non-heap pool imports fail explicitly.
- PS5 dirty tracking conservatively compares exact 4 KiB page snapshots. It
  detects writes through aliases and invalidation but cannot provide deferred
  GPU read guards. Device-local pool mirrors are therefore disabled. Snapshot
  allocation and scanning add costs that have not been measured in gameplay.
- Native static conversion uses astcenc 5.7.0 and bc7enc_rdo revision
  `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`, with their licenses packaged. The
  single AVX2 ISPC target uses the SysV ABI and 64-bit addressing. Its largest
  observed encoder stack frame is 160,704 bytes, so the standalone and bundled
  conversion suites request an 8 MiB worker stack. Production renderer workers
  use the existing RADV native stack wrapper.
- The core SDR presentation pass remains real. Optional HDR, FSR/NIS and
  temporal processing are excluded on PS5, consistent with the renderer's
  existing rejection of unsupported settings. Diagnostics record actual phase
  durations/counters/thread lifetimes; they do not claim OS stack sampling.

## Rebuild from source

First follow the pinned SDK, native runtime and partitioned RADV source recipe
in [GRAPHICS_PORT.md](GRAPHICS_PORT.md). Install the pinned **host** ISPC tool in
an explicit dependency directory; it is not a PS5 runtime dependency:

```sh
mkdir -p work/ps5/texture-tools
curl -fL https://github.com/ispc/ispc/releases/download/v1.32.0/ispc-v1.32.0-linux.tar.gz \
  -o work/ps5/texture-tools/ispc-v1.32.0-linux.tar.gz
echo 'faf266bda2b702d68976ffd26bbd36c1ddba43424cc3c44f25b092cd1aa1f64a  work/ps5/texture-tools/ispc-v1.32.0-linux.tar.gz' | sha256sum -c -
tar -xf work/ps5/texture-tools/ispc-v1.32.0-linux.tar.gz -C work/ps5/texture-tools
```

Append these options to `ps5/build_graphics_app.sh` after its isolation-manifest
argument (the other paths/hashes follow the graphics recipe):

```sh
-DPKP_GRAPHICS_RUNTIME_TEST=ON -DPKP_GRAPHICS_MEMORY_TEST=OFF \
-DPKP_GRAPHICS_NVN_LINK=ON \
-DPKP_ISPC:FILEPATH="$(pwd)/work/ps5/texture-tools/ispc-v1.32.0-linux/bin/ispc"
```

`textures.cmake` downloads and hash-checks both source archives; no local source
override is needed. For independent host conversion checks:

```sh
cmake -S ps5/tests/textures_suite -B work/ps5/texture-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPKP_ISPC:FILEPATH="$(pwd)/work/ps5/texture-tools/ispc-v1.32.0-linux/bin/ispc"
cmake --build work/ps5/texture-host --parallel 4
ctest --test-dir work/ps5/texture-host --output-on-failure
```

## Recorded validation

Host conversion: **49,981 checks passed**, including decoding BC7 with the
independent reference implementation. All NVN objects, the native replay
fixture and native conversion suite compile; full executable linking and
FSELF packaging/integrity inspection pass.

The console memory report `memory-api-1793823234-123.log` contains 35 cases with
one failure: the existing full-heap allocation case exceeded the payload
process's available direct memory. All five added snapshot/tracker cases
passed. The later import-view case is compiled but has not run on console.
This is not a passing full memory suite.

Standalone console conversion attempts did not complete. The second report,
`textures-runtime-1793823293-126.log`, stops after decoder initialization.
The explicit worker stack and finer stage logging were added afterward; the
cause and fix remain unconfirmed. No subsequent texture test is represented
as a console pass.

The game startup report `game-native-1793822433-91.log` completed SDK static
initialization and received success for `ISelfController:2000`, then reached
the 60-second diagnostic limit. It did not verify graphics-supervisor boot,
NVN initialization or a game frame. Next integration must join the renderer
to game startup, complete the remaining required services and establish real
game shader/texture coverage before gameplay or performance can be assessed.

The native bundle is packaged as `PPSA98732` with executable SHA-256
`85957050c073fa5be72555134763c86035cee69c852d6f5bc42607c2e59869af`.
The adjacent generated manifest records driver/source hashes and license files;
`console_verified` remains false until this entire batch runs successfully.

Installation is recorded in `work/ps5/nvn-bundle-install.json`: the stopped,
owned title's new executable and native libc were fully hash-verified, and
`nvn-bundle-metadata.json` records readback of the provenance and both new
licenses. The previous executable remains in a local rollback backup.
No automatic app launch was performed.

The standalone failed conversion process (PID 126) remained listed even after
Payload Manager acknowledged termination. Its owned payload slot is retained
until the process is confirmed gone. The transfer helper (PID 122) was closed,
its port was verified closed, and the other three owned payload slots were
removed. `renderer-batch-console.json` retains the unresolved ownership so the
next session can finish cleanup after a console restart.
