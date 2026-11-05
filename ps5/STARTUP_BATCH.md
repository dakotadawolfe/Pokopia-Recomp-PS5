# Native startup dependency batch

The previous native run stopped deliberately at the missing `aud:u` service.
It had loaded 3,596 shaders, reported 139 missing shader keys, and presented
one frame. It had not reached a verified menu or gameplay.

The next work is ordered around reaching an interactive game:

1. **Audio service and output.** Compile and register the existing renderer,
   audio-out and audio-in protocols. Add the PS5 AudioOut sink, POSIX events
   and thread integration. Carry upstream's output-control command and native
   Switch 2 revision support. Use the registered console clock consistently.
   Reject unsupported/manual and excess sessions without corrupting a live
   renderer. Coalesce the guest's 240-sample updates into 256-frame output
   blocks, with a bounded queue and stereo downmix.
   Implement `CreateTransferMemory` for the SDK work buffer: retain coherent
   service access while enforcing owner permissions, prevent heap shrink or
   remapping while borrowed, and restore access after the final reference.
   Keep audio/HID transfer objects alive across IPC's temporary copy handles.
2. **Shader correctness and observed coverage.** Read compute shared memory
   from control offset `0x64`, separately from local memory at `0x50`, in both
   the runtime and translator. Regenerate the existing private cache and the
   observed material entries. All 3,802 selected blocks passed Vulkan 1.3
   SPIR-V validation: 175 additions, including all 139 observed misses, and
   38 changed existing translations. This is not full-game shader coverage or
   proof of correct rendered imagery.
3. **Other startup dependencies.** Integrate upstream's 13 pure string/memory
   handlers and their leaf import dispatch. Preserve exact import counting.
   Register HID, power, shared fonts and offline network services together.
   Feed the real controller into HID; keep a stable POSIX service mapping
   behind its guest shared-memory alias. Package the existing licensed
   DejaVu fallback. Preserve the PS5 parental-control implementation and pass
   specific disconnected/empty-data results to the SDK unchanged.
4. **Combined native run.** Install the app and cache with full readback, then
   have the user launch it and select their existing folder. A menu, responsive
   controls, audible game output and gameplay still require this test.

## Evidence and scope

The automatic PS5 dependency payload passed **64 cases, zero failures**.
These exercise the real AudioOut device with silent samples, audio renderer
events and PCM processing, session failure/reuse, controller mapping and
shared-memory updates, import dispatch/overrides, and offline network objects
and error propagation. The native-module rejection test now uses the compiled
ABI version rather than assuming that version 4 is always invalid.
The transfer-memory cases cover 4 KiB subranges, all three owner permissions,
coherent service writes, final-reference restoration, overlapping leases,
heap shrink, handle exhaustion and the SDK's audio-open call through HIPC.
The owner remains Normal/Locked, following
[Atmosphere's transfer-memory semantics](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_transfer_memory.cpp).
The current POSIX implementation accepts heap-backed owner ranges; guest
`MapTransferMemory` and `UnmapTransferMemory` remain explicitly unimplemented.

Host audio and NVN2 control-field checks passed. Four shader selection/cache
tests passed. The native app builds locally; ELF conversion and the package's
exception-runtime checks pass. No automated native game launch is part of
these checks.

The import audit found 947 undefined SDK symbols in the main module and 357
in the secondary module. The main module includes 63 HID, 22 audio, 19 BCAT,
16 friend, eight network-interface and 12 SSL imports. Import presence is a
dependency indicator, not evidence that every function executes at startup.
Movie/codec paths, stream-play, album, dynamic loading and remaining display
operations still need runtime evidence and integration. Unhandled commands
continue to stop the diagnostic with a report; they are not blanket-success
stubs.

The PS5 sink currently accepts 48 kHz stereo or six-channel input and outputs
stereo. Its initial hardware buffering means a two-block write is not a valid
device-pacing test; the test uses 4,096 silent frames and repeat opens. Game
audio, rumble, motion controls and multi-controller use are not verified.
The fallback font has the limitations described in `runtime/src/services/pl.cpp`.

The startup deadline remains 600 seconds. Native libc handlers may reduce
startup time, but no game timing improvement is claimed before measurement.

## Installed batch

The app from source `501c576` and the 3,802-shader private development cache
are installed on the test console. Complete SHA-256 readbacks verified the
executable, licensed fallback font and every cache file after activation.
The previous executable and cache are preserved. Matching diagnostic symbols
are archived privately with a decompressed hash check.

The next manual test is to launch **Pokopia Graphics Test**, select the
existing decrypted game folder and choose **Use Folder**. Allow the combined
startup to run while collecting its report. Menu, game audio, controller
response and gameplay are still unverified with this batch; the component
checks do not substitute for that run.

## Reproducing component checks

The host checks contain synthetic inputs and need no game data:

```sh
cmake -S ps5/tests/audio_suite -B /path/to/audio-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/audio-host --parallel 4
ctest --test-dir /path/to/audio-host --output-on-failure
python -B -m unittest discover -s ps5 -p test_shader_cache.py -v
```

For the PS5 dependency payload, configure the same source with the documented
payload SDK toolchain and `PS5_PAYLOAD_SDK`, then build `ps5_audio_tests`.
Use `console_probe.py` to run it and retain its `audio-runtime` report; close
the owned transfer session after installation/collection. The sink test is
silent and does not start the game.

For private shader regeneration, use `prepare_startup_shader_corpus.py` with
`--runtime-log` and `--include-cache` pointing at the previous verified cache
manifest. Avoid `--expand-source-files` unless the resulting scope is intended.
The selector now refuses more than 10,000 shaders before copying by default.
After translation, `prepare_shader_cache.py --replace-existing` validates all
blocks, preserves binding metadata and records changed translations. All
outputs remain ignored private data.

For the app, use the existing [build/package workflow](GAME_RENDERER_STARTUP.md).
`replace_graphics_executable.py --install-shared-font` stages and reads back
the repository's DejaVu font and license before replacing the executable.
It preserves an existing different resource instead of overwriting it.

## Source and distribution boundaries

The selected upstream changes come from `drakolordx7/switch2-recomp` at
`5339bf85b05e84c34a7f18d8a43db1de1f1692b2`; the comparison is in
[UPSTREAM_REVIEW.md](UPSTREAM_REVIEW.md). NativeNX's audio and service notes
informed the audit. Its platform backends were not copied into this PS5 build.

Only implementation, synthetic tests and documentation belong in Git. Game
inputs, recompiled modules, executables, extracted shaders, SPIR-V caches and
runtime reports stay private. The development cache is still prepared on the
PC. A release that generates all game-derived outputs on PS5 from the user's
selected decrypted files remains outstanding, as does gameplay validation.
