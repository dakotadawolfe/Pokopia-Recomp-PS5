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

## First combined game run

The manual run reached the audio-device interfaces and opened AudioOut at
48 kHz stereo, then stopped deliberately at the missing `hwopus` service.
The native wrapper returned status 2 and the app process exited. This was
not a passing game run or an observed GPU crash. No audible game output,
interactive menu or gameplay was verified.

The measured guest startup interval was 551,462 ms, compared with 557,740 ms
before this batch. It loaded 3,735 shaders with zero missing entries, compared
with 3,596 loaded and 139 missing previously. Rendering remained at seven
submissions, one draw and one presentation. The private report is retained as
`game-native-1793853210-106.log` alongside its install/build receipts.

The selected assets were on USB; the shader cache was internal. All 1,554
completed synchronous IPC calls were timed: their handlers summed to 2,101 ms
and their diagnostic overhead to 1,330 ms. Shader lookups summed to 10,246 ms.
Those measured operations do not explain most of the nine-minute startup.
They are not a complete CPU or storage profile, and moving assets locally is
not yet a measured speed improvement. The 127,754,788 completed SleepThread
calls warrant investigation; their sampled timings must not be extrapolated
or treated as CPU utilization. Wait times sum across multiple guest workers.

The tracked host heap stayed at 192 MiB. The largest free direct-memory block
fell from about 690 MiB during loading to about 94 MiB at exit. This is not
total free memory, but warrants checking worker-stack and audio allocations
before extending startup. The existing worker path allocates 64 MiB stacks;
reducing them requires stack-use evidence rather than an arbitrary new size.

Next work, in order:

1. Implement real Opus decoding and audit the remaining codec startup calls.
   The upstream `misc.cpp` decoder currently outputs silence; registering
   that service alone does not provide working game audio.
2. Add per-stage CPU/wait and asset-read measurements, examine the yield loop
   and memory headroom, and display loading progress while the game loads.
3. Implement and validate the title-contained first-run install, USB-free
   subsequent launch, uninstall cleanup and native save-data requirements in
   [RELEASE_REQUIREMENTS.md](RELEASE_REQUIREMENTS.md).

Keep these changes in a combined iteration before requesting another manual
game launch. The current executable remains installed; it was not replaced
or relaunched during monitoring.

## Opus and startup diagnostics batch

The next app implements `hwopus` with pinned Xiph libopus 1.6.1, including
single-stream and multistream PCM decoding, the extended large-frame options,
packet length/final-range validation and explicit reset. A failed decode
preserves the output and decoder state. The service retains the client's
transfer-memory object until its decoder closes. The legacy context request
has no effect on bitstream state; it is not a fabricated decoded-audio path.
Newer, undocumented client-session commands remain unsupported and will stop
the diagnostic instead of reporting success.

The decoder dependency uses the official release archive and published hash
from [Xiph](https://opus-codec.org/downloads/). Its BSD license is copied into
the native package. The IPC parameter layouts follow
[libnx](https://github.com/switchbrew/libnx/blob/master/nx/source/services/hwopus.c)
and [Switchbrew's audio documentation](https://switchbrew.org/wiki/Audio_services).
The tests generate their own tones and packets; no game audio is included.

All 72 dependency cases passed on the test PS5, including stereo and six-channel
Opus output, malformed packets, work-memory lifetime, actual CPU-clock access
and point sampling within an owned worker stack. Host audio, shader-control,
RomFS and platform/thread checks also passed. These checks do not prove game
audio, menu or gameplay.

The combined app now reports cumulative host asset reads, actual bytes read,
open attempts, errors and elapsed read time. Metadata and padding generated in
memory are excluded. Concurrent read durations may overlap. Per-worker CPU
time is sampled on that worker at SVC boundaries, at most every 100 ms, with
sample age, guest call site and owned-stack usage. Old samples are explicitly
aged; these observations are not complete stack high-water coverage, and the
64 MiB worker stacks have not been reduced.

A native loading view runs on the existing render thread when its job queue
has been idle for one second. It shows elapsed time and asset bytes read, and
stops after a second guest presentation or the app's running notification.
Loading-view presentations have a separate counter and do not count as game
frames. This addresses the initial blank presentation without concurrent
access to the renderer. Visible behavior and startup duration still require
the next manual game launch.

This batch retains the existing development asset/cache locations. The final
title-contained installation, console-side preparation and native save-data
requirements remain outstanding; see [RELEASE_REQUIREMENTS.md](RELEASE_REQUIREMENTS.md).

The app built locally from `67d7a5d` is installed on the test console. Full
SHA-256 readbacks verified both the executable and the Opus license. The
previous executable and matching compressed symbols are retained privately.
The transfer helper and its payload slots were closed after installation.
The native game was not launched automatically; the next manual game run is
still required to verify progress beyond the codec startup boundary.

## Worker-stack memory batch

The subsequent manual run reached `hwopus` and completed two manager size
queries. At 588,593 ms, allocation of the 35th 64 MiB host worker stack failed
with errno 35. `StartThread` returned `0xce01`, the guest called `Break`, and
the native wrapper exited with status 2. This was an allocation failure before
the 600-second deadline, not verified game decoding, a menu or gameplay.

The 34 successfully started worker stacks alone accounted for 2,176 MiB.
The final largest free direct-memory block was 27,754,496 bytes; that value
is not total free memory. This compact native build now uses guarded 32 MiB
worker stacks, saving 1,088 MiB at the same worker count. The generic runtime
default and guest heap budget are unchanged. The game still needs to establish
that the reduced stack capacity is sufficient for its deeper call paths.

Opt-in stack painting adds owner-only touched-byte watermarks, scanned at most
once every ten seconds. These detect returned deep frames that SVC stack-pointer
samples miss. They do not prove every possible stack-pointer position or the
maximum future stack requirement. On PS5, two concurrent 32 MiB workers each
successfully touched and checked a 24 MiB local frame, detected it after return,
retained inaccessible guard pages, and released their backing after join.

The combined dependency payload completed 73 cases with one failure: the
AudioOut device could not open after the game run. All other cases, including
the new stack case and Opus tests, passed. A diagnostic rerun stalled inside
`sceAudioOutInit`; both stalled diagnostic processes were explicitly stopped
and their reports retained. Audio phase/error reporting now identifies init,
open and volume failures. A console restart and fresh audio-device check are
required before treating the next game run as comparable. Host audio,
shader-control and platform/thread checks passed; these do not replace that
device check.

The game run completed 1,422 host file reads totaling 105,899,418 bytes in
2,063 ms, with zero read failures. Shader lookup time was 10,624 ms, with
3,735 shaders loaded and zero missing entries. Rendering stayed at seven
submissions, one draw and one presentation. The native loading view updated
587 times, counted separately from game frames. Main-thread CPU samples
reached roughly 498 seconds; storage timings do not explain the long startup.
The yield-loop and native CPU cost remain investigations, not fixed bottlenecks.

The startup deadline is now 900 seconds so the next run can exercise stages
beyond the observed 589-second allocation stop. Extending that diagnostic
limit is not a startup-speed improvement. The native app built and packaged
locally with exception-linkage and FSELF checks. Game-derived artifacts and
all reports remain private. Installation and a subsequent manual game run
must still be verified separately.

After the console restart, the same dependency payload passed all 73 cases
with zero failures. AudioOut initialized, drained silent PCM and closed
successfully across three opens. The two guarded 32 MiB workers again passed
the deep-frame watermark and backing-release checks. This resolves the
component retest requirement; game audio and the game's maximum stack use
remain unverified.

The app built locally from `ea8b0db` is installed with the title stopped and
unmounted. Complete readback verified the executable and Opus license; the
previous executable and matching diagnostic symbols are retained privately.
The transfer helper and its diagnostic payload slots were removed. The game
was not launched automatically. The next manual run must establish whether
the worker-memory change passes the observed startup allocation failure.

## Offline GameShare startup batch

The reduced-stack manual run passed the previous allocation failure and
started 51 workers. At 637,594 ms it stopped deliberately on the missing
`stpl:u` service and exited through the native wrapper with status 2.
There were no worker-stack allocation failures. The final largest free
direct-memory block was 489,095,168 bytes; this is not total free memory.
AudioOut initialized successfully and serviced guest PCM buffers, but audible
game output and Opus decoding were not verified. Rendering remained at seven
submissions, one draw and one presentation. The run loaded 3,738 shaders with
zero misses; 1,432 file reads completed with zero failures in 2,080 ms.

`stpl:u` is the GameShare interface. Its offline startup protocol already
exists in the core's `runtime/src/services/misc.cpp`; the PS5 build did not
compile that platform-dependent service collection. The PS5 implementation
now provides the root/user interfaces, consistent offline work-size queries,
read-only empty guest-HID shared memory, disconnected state and events,
initialization/finalization, controller options and stored output volumes.
Repeated HID requests duplicate handles to one process-owned allocation;
closing one handle does not invalidate another handle or a mapped view.
Handle exhaustion returns an error with no bogus handle in the response.

This provides offline startup, not GameShare networking. Matchmaking, work
buffer submission and starting a stream remain explicitly unsupported. The
small offline work partitions follow the existing core; they are not real
streaming buffer requirements. Unknown commands still produce a diagnostic
failure instead of unconditional success.

Five focused service cases passed on PS5 with zero failures. They cover
real HIPC object/handle replies, shared-memory extent/content/permissions,
duplicate handle lifetime, bounded work-size inputs, disconnected events,
shared initialization state and finalization, malformed parameters, handle
exhaustion/recovery and rejection of streaming commands. This focused payload
does not reopen AudioOut after a game run. The full dependency payload also
builds with these cases included; its earlier 73-case device pass remains a
separate result. The updated app builds and packages locally with the existing
exception-linkage and FSELF checks. A game run against this service is still
required; no menu or gameplay is verified.

The app from `e525d0e` is installed with complete executable and license
readback verification. The previous reduced-stack executable and the new
matching symbols are retained privately. The owned transfer helper and test
payload slots were removed after installation. No automatic game launch was
performed; the offline service integration still needs a manual game run.

## Offline guest-display crash

The subsequent manual run reached the GameShare service queries and then
crashed after about 648 seconds. The kernel report identified a null read in
the guest SDK while creating individual GameShare displays. Matching private
symbols and the generated call chain traced it through VI2 initialization to
an absent GraphicsSupervisor session. The PS5 NVN bridge deliberately owns
graphics instead of starting that guest driver; the missing piece was the
GameShare display handoff already present in `runtime/src/core/switch2.cpp`.
The asset-read counters recorded no failures and about two seconds of host
read time; this crash is not evidence of slow USB reads.

The PS5 bridge now handles individual-display creation, native-window lookup,
destruction and both guest HDR metadata calls. Offline guests borrow the
existing native window identity, with bounds and lifecycle validation. They
do not create remote displays or network connections. Guest metadata leaves
the primary display settings alone, and destroying the primary layer clears
the borrowed identities. The SDK export preflight still rejects missing
symbols before changing routing. Unknown streaming operations still fail.

The focused payload passed all seven cases on PS5: the five service cases
plus native-window lifetime and invalid-setup coverage. The native app also
builds locally. These checks do not execute the game's startup path or prove
menu/gameplay; the corrected app still needs a manual integration run.

The app from `5e263a3` is installed with complete executable readback
verification. Its matching symbols and the previous executable are retained
privately alongside the crash evidence. The transfer helper and diagnostic
payload slots were removed afterward; no automatic game launch was performed.

## Account-save startup batch

The next manual run passes the offline display bridge and deliberately stops
at the game's save-data setup request, then exits normally to Home. The next
batch adds persistent account-save storage, filesystem operations, quota and
size handling, commit recovery and expected first-boot error handling. All 36
filesystem/service cases pass on PS5. Native storage and the next game startup
remain pending the manual integration run. See [save storage](SAVE_STORAGE.md)
for the development policy, evidence and remaining Saved Data integration.

## Save-stage shaders and diagnostic overhead

The corrected native run passes the title-storage checks and creates the
actual game's account save. Private report `game-native-1793904273-113.log`
then reaches the 900-second watchdog and exits normally through the native
wrapper with status 2. Its final profile is at 915,348 ms; concurrent workers
continue logging briefly during shutdown. Rendering remains seven submissions,
one draw and one presentation, with no guest frame verified.

The full report contains 1,298 distinct missing shader hashes. Of these, 1,086
match the verified static corpus; selecting their related material entries
produces 2,910 shaders. All translate successfully and pass Vulkan 1.3 SPIR-V
validation. Merging them with the previous 3,802-entry cache yields 6,704
validated shaders. The remaining 212 are not present in that index. A scan of
the executable modules and previously excluded archive-entry types did not
recover them. Their origin is still unresolved; translations are not guessed
or substituted. The indexed subset and its original-report provenance remain
in private receipts.

The next app captures missing code/control pairs under the title's
`/download0/pokopia-shader-capture`, with a 16 MiB per-run write budget and
checked file writes. This is private diagnostic data, not a public asset pack
or the finished on-console compiler. Capture in the native sandbox still
requires the next run. Existing captures may remain between runs; the budget
is not a total-directory quota.

The run completed 1,515 asset reads, 115,292,610 bytes, in 2,218 ms with no read
failures. Cache lookups and diagnostic time increased later in startup. The
observer also copied every service's statistics after each synchronous IPC.
It now checks a small registry flag first, retaining detailed reports and
stopping on unknown commands or services. Ordinary request logging and
repeated SVC results retain initial calls and periodic counts instead of a
line per audio buffer. Request inspection remains serialized, and summed
observer time includes contention; it is not a CPU-cost measurement or proof
of a loading-speed improvement.

All 69 IPC component cases pass on PS5 in private report
`ipc-runtime-1793905837-116.log`, including unknown-command/stub detection and
registry reset. Five host shader-selection/installation checks pass. The app
builds locally; installation and another native run are tracked separately.
Normal Saved Data integration, title-contained import and on-console shader
compilation remain release work. All game-derived artifacts stay private.

The combined iteration is installed with the title stopped. Full readback
verifies all 6,704 cache records and the app from source `c336496`:
279,846,105 bytes, SHA-256
`9b5d7ffccf975b13477a0a1647320112d8307bfb51f7409084338ebba2bec548`.
The previous cache/executable, matching symbols and private provenance are
retained. The owned helper and diagnostic slots were removed afterward.
No automatic game launch was performed. The next manual run must establish
whether this reaches a game frame and whether native shader capture works.

## Captured save-stage shaders

Private report `game-native-1793929601-131.log` verifies native shader capture:
all 212 previously unindexed misses were recovered from the live title's
`/download0/pokopia-shader-capture`. Each code/control pair was downloaded
before the sandbox unmounted, checked against its recorded sizes and code
hash, and preserved in an immutable private snapshot with SHA-256 identities.
All 212 translate successfully and pass Vulkan 1.3 SPIR-V validation. The
merged cache contains 6,916 validated blocks; its provenance distinguishes
runtime captures from static source-entry selection. This covers the observed
misses, not every shader in the game, and does not prove rendering correctness.

The run reopened the game's account save and ended at the 900-second watchdog
with native status 2. It loaded 5,018 shaders and reported 212 misses. The final
asset counters are 1,521 reads, 116,177,619 bytes, 2,259 ms and zero failures.
Shader lookups total 12,826 ms, with a 20 ms maximum. No guest frame appeared:
the seven submissions, one draw and one presentation are still initialization.
The largest contiguous direct-memory range was 220,659,712 bytes; that is not
total free memory. No new unsupported-service stop was reported.

Shader loading advanced from 4,219 to 5,016 blocks around 883 seconds, shortly
before the old deadline. The next app permits a bounded 1,800-second diagnostic
run so continued initialization can be observed. This is additional diagnostic
time, not a startup-speed fix. Five host cache-selection/installation checks
pass, and the native app builds locally. Deployment and the next integration
run remain separate checks. Captures, translations, reports and binaries remain
private; the release still needs on-console compilation from user inputs.

The captured-shader iteration is installed with the title stopped. Complete
readback verifies all 6,916 shaders plus metadata and the app from `0ca157f`:
279,846,105 bytes, SHA-256
`1daf5b03d37a1611e3c7d776a6ea41842f7fb8a525122c6286cadf0830061fd8`.
The previous cache and executable, matching symbols and capture provenance
remain private. The owned transfer helper and its payload slot were removed,
and FTP port 2121 was verified closed. No automatic app launch occurred; the
expanded-cache integration run remains pending.

## First-render startup batch

The subsequent private report `game-native-1793931637-134.log` reaches
presentation calls 2 and 3 and background texture preparation around 1,124
seconds, then ends without a controlled-stop marker. Visible game imagery and
the crash cause remain unverified. All 378 new missing shader hashes were
captured; the retained folder also includes the previous 212 captures. All
590 translate and pass Vulkan 1.3 SPIR-V validation locally.

The last renderer message exposes a separate clear-path error: stencil mask
`0xffffffff` is classified as partial despite selecting all eight stencil
bits. The renderer now normalizes the mask to eight bits before deciding
between an attachment clear and a masked draw. The native app builds locally;
this correction is not yet console-verified or established as the crash fix.

The combined update is installed with full readback verification: 7,294 shader
blocks plus metadata and the app from `f441e32`, SHA-256
`27918d3e392960f42ec9a8bbf97e955469f436e8d9e02f0777083f2bd02354f0`.
The previous cache and executable and matching symbols remain private. Five
host cache checks pass. The owned transfer helper was removed after installation.
The next run should capture the kernel log during startup: the retained ring
buffer from the previous failure did not contain the fault. No automatic launch
occurred, and rendering/gameplay validation remains pending.

## Descriptor allocation and worker memory

The next batch refuses draws and dispatches when a required descriptor set
cannot be allocated. Previously those operations could continue with stale or
unbound sets. Descriptor-pool failures now record the Vulkan result; only pool
exhaustion and fragmentation retry in a fresh pool. This fixes a source-level
error path, but does not establish that allocation failure caused PID 151's
GPU fault.

The diagnostic uses guarded 8 MiB worker stacks instead of 32 MiB, retaining
owner-only stack watermarks. PID 151's largest observed touched stack was
14,648 bytes; that is startup evidence, not a bound on future gameplay. The
change saves 24 MiB per worker. Both the existing 32 MiB/24 MiB frame test and
a new 8 MiB/6 MiB frame test pass on PS5, including concurrent workers, returned
frame detection, guard pages and backing release. Private report
`audio-runtime-1793945253-157.log` records 81 cases and zero failures.

The native app builds locally. Early NVN frames 0 through 5 now log their
operations and bindings, so the next live kernel fault can be correlated with
the renderer. These traces are private game-derived diagnostics. No new shader
cache is needed: the previous run had no missing shader records. Native startup
and the descriptor failure path still need integration validation.

## Reproducing component checks

The monitored follow-up (native PID 151) still fails during renderer startup,
around 1,115 seconds. Its last shader progress is 5,512 loaded, zero missing;
the complete report contains no missing-shader records. Presentation reaches
calls 2 and 3 before texture preparation starts, but game imagery is unverified.
Live kernel capture identifies `GPU_FAULT_PAGE_FAULT_ASYNC` (`0xa0d0c005`):
SQC Data attempts a read at unmapped GPU VA `0x200000000`. This is the base of
the driver's separate 32-bit window, not proof of which descriptor or instruction
is responsible. Stencil normalization did not resolve the crash. The monitor's
generic SIGSEGV match was from the restarted ShellUI's capability listing;
the app's recorded fault is the asynchronous GPU page fault above.
The report, 105 compressed kernel snapshots and fault receipt remain private.
The owned helper was removed after collection. No rerun was requested.

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

The same PS5 build also provides `ps5_streamplay_tests`, containing five
offline GameShare service cases and two offline guest-display cases without
the audio-device test. Collect its
`streamplay-runtime` report with `console_probe.py`.

For private shader regeneration, use `prepare_startup_shader_corpus.py` with
`--runtime-log` and `--include-cache` pointing at the previous verified cache
manifest. Avoid `--expand-source-files` unless the resulting scope is intended.
The selector now refuses more than 10,000 shaders before copying by default.
After translation, `prepare_shader_cache.py --replace-existing` validates all
blocks, preserves binding metadata and records changed translations. All
outputs remain ignored private data.
For immutable caches on one filesystem, `--hardlink` avoids copying SPIR-V
files again while retaining full validation and a new metadata manifest.
Never edit those linked files in place; use a separate translation output.
The installer accepts `--jobs 4` for independent file transfers. Directory
checks, staging, full readbacks and the stopped-title activation remain ordered;
a failed worker prevents publication. The default remains one connection.

For the app, use the existing [build/package workflow](GAME_RENDERER_STARTUP.md).
`replace_graphics_executable.py --install-shared-font` stages and reads back
the repository's DejaVu font and license before replacing the executable.
It preserves an existing different resource instead of overwriting it.
For the Opus batch, also pass `--opus-license` with the package's
`licenses/opus-BSD-3-Clause.txt` and `--opus-license-sha256` with that entry's
hash from `graphics-app-manifest.json`. The installer verifies and stages
the license before activating the new executable.

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
