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
These are virtual address ranges, not extra physical memory. The combined app
now reports a fixed 9 GiB guest application budget, including mapped images and
process overhead. The independent 11 GiB heap-request threshold and
pre-allocation check for 1 GiB remaining host headroom remain enabled. Combined
game/GPU working-set capacity still needs console evidence.

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
The initial combined candidate is 278,049,217 bytes with SHA-256
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
launch was performed automatically.

## Native exception linkage correction

The first combined run, `game-native-1793827615-101.log`, reached the host
nonlocal-jump check and several guest exception checks, then crashed in
`host_exception_removes_nested_guest_catch_frames`. It did not reach the folder
picker, NVN backend installation or game dispatch. The synthetic guest error
messages before that final case are expected test inputs, not game failures.

Binary comparison identified mixed exception linkage: the earlier successful
native startup app defines `__gxx_personality_v0` and the C++ exception globals
locally, while the combined build imported them but linked its throw/unwind
functions statically. The graphics link now searches the real static C++
runtime before system import stubs. The SDK libc archive stays after system
imports so payload-only syscall adapters cannot displace native kernel imports.
The native and RADV SDK copies of libc++, libc++abi and libunwind must all match.

Packaging now requires all nine core C++ exception functions to be defined
locally, in addition to the qualified nonlocal-jump implementation and aliases.
The corrected ELF passes these checks; a negative fixture with its personality
symbol renamed is rejected and removed afterward. The subsequent console run
below confirms the exception tests pass with this linkage.

The correction is 278,224,929 bytes with SHA-256
`79164e2877f3c7252dc335cb03f954b583c9c44b7d2f2f849348ae5cb5ec6656`.
Native ELF: 284,010,836 bytes, SHA-256
`897f65515e6594e5165aba6f2d35565f4b7f17647139b505d1668c94c7bd21c9`.
The correction is installed after ShadowMountPlus confirmed the title was
stopped and unmounted. `game-renderer-eh-install.json` records the previous
executable backup and complete SHA-256 readback of both staged and installed
files. The existing startup shader cache and app provenance are unchanged.
The crash-report helper (PID 104) and replacement transfer helper (PID 93) were
closed and their payload slots removed; `game-renderer-result-console.json`
and `game-renderer-eh-console.json` record cleanup.

## Startup visibility and trace overhead

Report `game-native-1793828278-96.log` confirms zero exception self-test failures,
accepted folder selection, four mapped/registered game images, 44 RomFS files
covering 7,246,055,230 bytes, and installation of the native graphics bridge.
The renderer continued displaying the last picker frame while guest SDK startup
ran. The user saw an apparently stuck picker followed by Home. The report
instead records the 180-second watchdog, `GAME_TRACE_END`, and native exit
status 2. It reached about 135 system calls, with no title graphics
initialization, GPU submissions, draws or game presents. `nninitStartup` in the
main game module was not reached. This is progress past the earlier host
exception crash, not a menu or gameplay result.

The combined diagnostic previously used unbuffered stdout/stderr and printed
entry and exit details for every system call. It now uses static line buffers,
retains the first entry/exit for each system-call number, and retains error
results and named IPC requests. The GetSystemTick return is a tick value, so
its nonzero result does not generate a false error trace. Other diagnostic
targets retain their existing trace mode. Runtime rejection checks, memory
headroom, the system-call limit and the 180-second deadline are unchanged.

Fixed atomic counters measure completed handler time separately from the
surrounding diagnostic work. Every ten seconds `GAME_STARTUP_PROGRESS` reports
elapsed time and completed/active calls. `GAME_SVC_PROFILE` at stop includes
per-call-type timings and the latest entry addresses. These are approximate
concurrent snapshots and summed wall times across workers, not CPU utilization
or per-thread backtraces. Console measurements are still required to establish
how much of the slowdown is trace overhead versus guest/runtime work.

After confirmation, the renderer replaces the picker with `STARTING POKOPIA`.
It displays asset indexing, module checks, bridge setup and game-service startup
as those steps execute on the loader thread. It does not draw from the watchdog
or compete with guest presentation after dispatch. There is no synthetic game
image or invented progress percentage.

The updated candidate passes the native build, exception/jump linkage gates,
FSELF packaging/inspection and the existing host folder-picker suite. FSELF:
278,327,169 bytes, SHA-256
`869494c5e6a9c43264e49f47bf042bd6c341abb8e27071266858db8addec977a`.
Native ELF: 284,125,588 bytes, SHA-256
`a76bc02ac695779e432a9e8ce55e7e030679479297fc6d39b97ef9cbeb194ba0`.
The candidate is installed after ShadowMountPlus confirmed the owned title
was stopped and unmounted. `game-renderer-progress-install.json` records the
previous executable backup and complete SHA-256 readback of staged and installed
files. The startup shader cache and app provenance are unchanged. The transfer
helper (PID 91) was closed, its slot removed, and port 2121 verified closed;
`game-renderer-progress-console.json` records cleanup. The following run records
this candidate's console result.
The report-collection helper (PID 97) was closed and its slot removed;
`game-renderer-picker-console.json` records cleanup.

## Native graphics memory budget

`game-native-1793829587-92.log` confirms the startup trace correction on console:
guest dispatch reached the main module's `nninitStartup` and its heap request
in 158 ms, at system call 142. Completed service handlers accounted for 15 ms
and diagnostic work for 57 ms (summed across workers). The earlier three-minute
trace had stopped before this point. No game frame was produced in either run.

The new stop is explicit capacity accounting. The inherited 10.5 GiB Switch 2
resource pool caused a heap request of 11,123,294,208 bytes. The largest native
free block was 11,991,515,136 bytes, leaving only 868,220,928 bytes after that
request, below the unchanged 1 GiB reserve. No allocation was attempted; the
application returned status 2 normally. The report helper (PID 94) was closed
and removed; `game-renderer-fast-result-console.json` records cleanup.

Inspection of the private compiled initializer at `0x71000a44e0` confirms that
it subtracts reported used memory from available memory and rounds the result
down to 2 MiB. The combined native graphics target now sets its fixed resource
pool to 9 GiB through the existing `memory.app_memory_mb` accounting path before
guest startup. GetInfo, GetSystemInfo and SetHeapSize therefore agree. The port
does not truncate allocations, patch the private game module, weaken the free
memory guard, or add a user-facing graphics setting. Other targets retain their
existing resource limits.

With the same initial accounting, the projected heap is 9,512,681,472 bytes and
the projected remaining block is 2,478,833,664 bytes. Those are projections from
the captured run, not measured game working-set requirements. The logs now
include `GAME_MEMORY_PROFILE` and `GAME_HEAP_BUDGET` so the next native run can
confirm the actual request and remaining capacity.

The rebuilt service payload ran on PS5 as
`svc-runtime-1793829854-98.log`: **126 cases, zero failures**, including budget
query agreement, changing used-memory accounting and rejection of oversized
requests before backing allocation. Payload SHA-256:
`d1a0c6074000248bd4777f855651db9cdfd62f6d459dec73d177fa9148c60df4`.
This validates the service path, not the game's full native working set.

The native build, exception/jump linkage gates and FSELF inspection pass.
FSELF: 278,327,745 bytes, SHA-256
`fd5504fb927dee336d9b94b7a78da0f15d9c5e4e407b44f68667632c6fe22ee9`.
Native ELF: 284,125,588 bytes, SHA-256
`8c024a79a67252dec54ea5c206d6008a063d0f8ff328545cfd4d6a87aa4e0e10`.
The candidate is installed with the owned title stopped and unmounted.
`game-renderer-memory-install.json` records the previous executable backup and
complete SHA-256 readback of staged and installed files. The updated provenance
was separately backed up and readback-verified in
`game-renderer-memory-metadata.json`; startup shaders and the selected game
folder are unchanged. The transfer helper (PID 97) and completed service-test
payload (PID 98) are gone, both owned slots were removed and port 2121 is closed.
`game-renderer-memory-console.json` records that cleanup.

Report `game-native-1793830091-102.log` now confirms the revised budget in the
native app. The actual 9,512,681,472-byte heap allocation succeeds in 586 ms and
`nninitStartup` returns. The game then calls the real PS5 graphics initialization
bridge: `GAME_GRAPHICS_READY backend=PS5-NVN`. Guest dispatch reaches this point
and its next stop in 773 ms. The stop is the unimplemented ICommonStateGetter
command 66, not a memory failure or native exception. The report ends normally
with status 2. Submissions, draws and presents are all zero; no menu or game
image is established by this result.

## Applet startup and native display batch

The PS5 application service now handles the normal/fast-load CPU hints and
cancellation, application-running notification, default display resolution and
its change event. The command layouts follow the primary
[libnx applet client](https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c)
and the normal/fast-load values in
[libnx APM definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/apm.h).
This is a fixed native clock policy: accepting the guest's loading hint does
not overclock the PS5 or reproduce Switch CPU/GPU frequency changes.

Loading and running state belongs to the application, persists across sessions
and resets independently of old session handles. Invalid modes, short inputs,
foreign handles and unexpected buffers fail before mutation. The scalar input
checks allow CMIF alignment padding rather than treating it as extra arguments.
The running notification acknowledges application lifecycle state; it is not
evidence that the game reached its menu.

Display dimensions come from the initialized native renderer, before guest
dispatch, and are refreshed after presentations. Queries fail until a valid
size is supplied. Event handles share an actual event; a changed size signals
it, while initial setup and duplicate sizes do not. Reset isolates previous
sessions and event readers. The native report adds `GAME_DISPLAY` and
`GAME_APPLET_STATE` to distinguish renderer output and guest loading policy.

The full service payload passes **130 cases, zero failures** on PS5 in
`svc-runtime-1793830400-117.log`, including four added cases covering these
operations, invalid requests, event clearing and reset isolation. Payload
SHA-256: `4547b6282ffe7a6e13815328ebb81c9a16bf0ec30370640ac46c205a53126c00`.
The native build, exception/jump linkage gates and FSELF inspection also pass.
FSELF: 278,341,889 bytes, SHA-256
`2a99fc4e43e346e6d221ce4a6f32ba83e328049586c921da21757fa212c0e56e`.
Native ELF: 284,127,388 bytes, SHA-256
`552a222d69ee2ec1fd08bd025eefccfe21c5fae3a4d13b38a2e8a09677ec4a26`.
The candidate is installed after confirming the owned title was stopped and
unmounted. `game-renderer-applet-install.json` records the previous executable
backup and complete staged/final SHA-256 readback. Package provenance and
startup shaders are unchanged; no game assets were transferred. The collection
helper (PID 104) and service-test process (PID 117) are gone, both owned slots
were removed and port 2121 is closed. `game-renderer-budget-result-console.json`
records their cleanup.
Game execution beyond command 66 still requires a native app launch.

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
