# Upstream review after the shader-memory run

The subsequent implementation and component checks are recorded in
[STARTUP_BATCH.md](STARTUP_BATCH.md). The review below describes the baseline
before that integration.

Reviewed 2026-11-04 (America/Chicago). This is a source comparison and a record
of the PS5 run, not validation of these upstream changes on PS5.

The monitored native app exited deliberately on the unimplemented `aud:u`
service after 557,740 ms. It passed the previous shader-allocation failure,
loaded 3,596 shaders and recorded 139 missing shader keys. Rendering remained
at one draw and one presentation. See [GAME_RENDERER_STARTUP.md](GAME_RENDERER_STARTUP.md).

## Sources and revisions

- The original `drakolordx7/pokopia-recomp` repository now resolves to
  [drakolordx7/switch2-recomp](https://github.com/drakolordx7/switch2-recomp).
  Our initial base is `2c29515b23a6d78d967775fd7e9f24f50402cca7`.
  The fetched head is
  [`5339bf85b05e84c34a7f18d8a43db1de1f1692b2`](https://github.com/drakolordx7/switch2-recomp/commit/5339bf85b05e84c34a7f18d8a43db1de1f1692b2).
  It changes 234 files, including the Windows launcher and platform-specific
  rendering code; it is not a PS5 integration patch.
- [Smithy-Lab/NativeNX](https://github.com/Smithy-Lab/NativeNX/tree/31d7be07621bef8826ce3c8c7e7a02a789c45f02)
  was reviewed at `31d7be07621bef8826ce3c8c7e7a02a789c45f02`.
  Its host uses ScarletNX recompilation, nxnative service code, Vulkan and SDL3.
- NativeNX's underlying [seedds/nxnative](https://github.com/seedds/nxnative/tree/cc60a0c1d749dadfdf6fb052db1b48194f2b95af)
  was reviewed at `cc60a0c1d749dadfdf6fb052db1b48194f2b95af`, including its
  Pokopia notes, audio implementation and service reference.

## Highest-value work for this port

| Priority | Finding | PS5 integration needed |
|---|---|---|
| 1 | The original runtime already implements `aud:u` in `runtime/src/services/audio.cpp`, but `ps5/svc.cmake` does not compile the audio services and the native startup in `ps5/tests/test_game_images.cpp` does not register them. The new upstream revision also handles output-control command 3 and Switch 2 native audio revision tags. | Port the renderer, audio output and event/pacing dependencies together, then register the real service. Adding a success-only service object would merely move the failure. |
| 2 | `runtime/src/core/libc_hle.cpp` replaces imported string/memory routines with host functions. The matching dispatch changes bypass import bookkeeping for leaf functions; `nxmodule.cpp` batches the shared import counter. Upstream identifies a Pokopia startup string-comparison loop as the motivation. | Adapt and test the whole import/override path, including our diagnostic `__wrap_host_import`. Measure the same startup workload; the upstream timing is not a PS5 result. |
| 3 | The shader control reader corrects NVN2 shared-memory size from offset `0x50` to `0x64`. The translator includes hardware-checked instruction fixes, including floating comparisons/rounding, half operations and memory/texture decoding. | Validate against user-supplied controls and regenerate only private outputs with preserved binding metadata. Check device capabilities for added shader operations. Compilation success alone does not establish correct rendering. |
| 4 | Upstream adds a shader worker, cache-record insertion and pending-program resolution. Programs can recover when a requested shader becomes available. This directly addresses our incomplete-cache model. | Preserve our bounded shader-word ownership. Replace the Windows process/pipe transport and provide a compiler that runs on PS5. The current worker starts a Windows .NET harness and is not an on-console implementation. |
| 5 | Upstream shortens specific Pokopia save/boot polling sleeps using version-specific return-address offsets. | Verify the module identity and call sites before adopting. Our measured hot sleep was the negative-duration yield at `main+0x1ff69b8`; it is not one of these positive-duration polling calls. This patch does not by itself explain that loop. |

The Switch 2 clock conversion in upstream `kernel_sync.cpp` is already present
in our PS5 branch. Reapplying it is not a new fix. The newly added DLSS,
Windows setup/archive import, PC mouse UI and other title features do not
resolve this run's immediate stop.

## Audio references and platform boundaries

Our existing renderer integration uses `nxrt::audio::sink()`. Its
`audren/Integration/ThreadBoost.h` currently includes Windows priority and timer
APIs, and the output backend also needs a PS5 implementation. Both the service
protocol and the output clock must be considered when bringing it into the app.

[NativeNX's PCM output](https://github.com/Smithy-Lab/NativeNX/blob/31d7be07621bef8826ce3c8c7e7a02a789c45f02/compat/nxnative/pcmout.c)
is a useful reference for buffer consumption, release queues and paced silent
tests, though its real device output uses SDL3. The
[nxnative audio notes](https://github.com/seedds/nxnative/blob/cc60a0c1d749dadfdf6fb052db1b48194f2b95af/docs/reference/audio.md)
describe renderer work-buffer sizing, per-renderer lifetime, release-event
ordering and stop behavior. These provide contracts to check; the Apple
CoreAudio/dispatch code is not directly usable on PS5.

The [nxnative Pokopia notes](https://github.com/seedds/nxnative/blob/cc60a0c1d749dadfdf6fb052db1b48194f2b95af/dm/games/0400A3A01EE4C000/README.md)
report first-area gameplay on Mac with a selected shader library, plus shader
translation and texture-storage fixes. Those are useful comparison cases, not
proof of PS5 performance. Their service reference recommends auditing imported
SDK entry points before another run; batch likely service dependencies with
audio instead of waiting for one missing service per manual launch.

## Release boundary and current state

No upstream runtime changes were merged as part of this review, and no new
native app was installed or launched. The installed app remains the bounded
shader-memory build from source commit `bdf7ca6`.

All game-derived logs, shader words, caches and recompiled executables stay
private. A public release must generate its game-derived outputs on PS5 from
the user's selected decrypted files. Neither the Windows on-demand worker nor
the current private development cache satisfies that release requirement.

The next substantial integration batch should combine audio service/output,
the imported-function startup improvements, shader correctness/coverage and an
audit of remaining imported services. Validate host and payload components
before asking for another native game run.
