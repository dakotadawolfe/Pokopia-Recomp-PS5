# Native game and renderer integration

This opt-in build joins the recompiled game modules to the PS5 Device/Renderer
and NVN backend qualified by [the native renderer batch](NVN_RUNTIME_PORT.md).
It is a bounded startup diagnostic, not a gameplay-qualified application.
Its executable contains game-derived code and its translated shaders are
private data. Neither is a public release artifact. The final release must
prepare both on the user's PS5; see [release requirements](RELEASE_REQUIREMENTS.md).

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
further translation. At this initial preparation stage, no game shader had
been exercised on PS5. The later first-presentation report is recorded below.

To extend coverage from a native report, select its missing hashes and the
other shaders in their first-source material entries, then translate that
batch with the same harness command above. The selector verifies each source
blob against its content key before copying. Expanding every occurrence of
shared shaders would unnecessarily include thousands of unrelated materials.

```sh
python ps5/prepare_startup_shader_corpus.py \
  work/ps5/shader-corpus work/ps5/observed-corpus \
  --runtime-log work/ps5/game-native-report.log
dotnet tools/shaders/build/bin/Release/net10.0/spirv_harness.dll \
  --batch work/ps5/observed-corpus --out work/ps5/observed-spirv \
  --jobs 8 --target-env vulkan1.3 --spirv-val /path/to/spirv-val
python ps5/prepare_shader_cache.py \
  work/ps5/startup-spirv work/ps5/startup-shader-manifest.json \
  work/ps5/observed-spirv work/ps5/observed-corpus/selection.json \
  work/ps5/combined-spirv --spirv-val /path/to/spirv-val
python -B -m unittest discover -s ps5 -p test_shader_cache.py -v
```

The merge requires the previous cache's file manifest, checks every translation
result, reruns SPIR-V validation, preserves binding metadata and rejects
conflicting duplicate hashes. Its output includes a new file manifest and
selection provenance. With the transfer helper open and the title stopped,
install it using the receipt from the prior successful cache installation:

```sh
python ps5/install_shader_cache.py --host CONSOLE_IP \
  --cache work/ps5/combined-spirv \
  --previous-receipt work/ps5/previous-shaders-install.json \
  --receipt work/ps5/new-shaders-install.json
```

The installer checks title ownership and ShadowMountPlus state, verifies all
existing files against that receipt, stages the complete cache and reads it
back before switching directories. It retains the prior directory as a named
backup, restores it if publication fails, and verifies every installed file.
It refuses unknown existing files instead of deleting or silently dropping
them. Keep installation receipts and backups for later updates or rollback.

If the report shows successive entries from the same archive still loading,
add `--expand-source-files` to the selection command. This selects all shader
occurrences from the first-source files of the observed misses. It remains
bounded to those source files and does not claim full-game coverage. The same
translation and merge commands apply; use the currently installed cache's
manifest and the preceding successful installation receipt as the baseline.

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
The subsequent native report, `game-native-1793830958-126.log`, still stops at
command 66, now with `0xee01` (out of range). The new handler was reached, but
its initial validation admitted only the older service values 0 and 1. The
134-case correction described below has not yet been game-tested.

## Supplied SDK wire values and expected polling results

Inspection of the actual supplied SDK explains the failed applet batch. Its
public `nn::oe::SetCpuBoostMode` at `0x7500516620` accepts API values 0 and 1,
then converts 1 to **service value 2** before calling the applet API. The applet
wrapper at `0x75001c5dc0` admits only 0 and 2 (`tst w0, #0xfffffffd`) and forwards
that value to the common-state service. The previous libnx reference describes
an older direct wire value; it was insufficient to validate this title's SDK.

These are static observations from the private compiled source and local
Capstone disassembly of the hash-verified NSO text, recorded in
`work/ps5/sdk-cpu-mode-evidence.txt`. SDK SHA-256:
`7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2`.
Text SHA-256:
`a88ae2da4f7c8507f5d50a35ac81a2ae8cec6a7e80b49df37c4d1e9048cd408c`.
The runtime stop identifies an out-of-range request; that older report did not
capture its argument bytes. The value-2 explanation follows from the supplied
SDK's validated call path, not a recorded argument dump.

The corrected handler accepts 0, legacy 1 and this SDK's 2, retaining the fixed
native clock policy. Its regression test exercises command 66 through a real
CMIF domain envelope with a four-byte input, including reset/cancellation and
rejection of value 3. This covers the protocol used by the game in addition to
the existing plain-session tests.

The startup observer now snapshots at most 16 raw argument bytes before the
reply overwrites TLS. A fatal IPC report includes that prefix and the call
addresses. Domain lengths exclude alignment and object IDs; plain CMIF lengths
are explicitly marked inexact because they can contain padding. Malformed
headers, bounded copying and retaining bytes across overwrite are tested.

Two already-ported empty queues also no longer force a diagnostic exit:
ICommonStateGetter command 1 with result `0x680`, and IPurchaseEventManager
commands 3/4 with result `0x320a4`. Their original results reach the SDK
unchanged. This is not a general error bypass: the same result on another
endpoint/command, malformed replies, missing game data, invalid inputs and
unimplemented operations still stop. Tests use the production service results
and verify those negative cases.

Final console service report `svc-runtime-1793831288-154.log` records **134
cases, zero failures**. Payload SHA-256:
`d636bc15b9dc8ad077296ed4e38df1e9484d3e7e1e016ee696199223304b047a`.
The native build and packaging/linkage checks pass. FSELF: 278,343,361 bytes,
SHA-256 `7b92f219069ffed9a1ab511ddca99e934e2f40c2e57888af8ab3c4b39d97f5ee`.
Native ELF: 284,127,388 bytes, SHA-256
`6a242a065e54c02ce34e42f63e4face91b53590ce8ccb3bceb13a272138e99c4`.
`game-renderer-wire-install.json` records installation with the owned title
stopped/unmounted, the previous executable backup, and full staged/installed
readback hashes. Provenance, startup shaders and selected game files are
unchanged. The helper (PID 128) and both service-test processes (PIDs 135/154)
are gone; their three owned payload slots were removed and port 2121 is closed.
Cleanup is recorded in `game-renderer-applet-result-console.json`.
Report `game-native-1793832208-89.log` now confirms that command 66 succeeds:
`GAME_APPLET_STATE loading_mode=2`. The game counts add-on content, connects
to `fsp-srv`, binds its process and asks for the global filesystem access-log
mode (command 1005). That unported query stops the run with `0x177202`; its
captured domain request has zero input bytes. Dispatch reaches this stop in
783 ms and exits normally with status 2. Native graphics are initialized, but
draws and presentations remain zero.

## Filesystem startup and read-only storage batch

The next batch implements the disabled access-log policy, automatic-save
creation disable request, and the single mounted program's index/count. It also
adds read-only storage flush, range information and real handle-cache
invalidation. It does not acknowledge writes or unknown range operations. See
[the RomFS service contract](ROMFS_PORT.md) for the wire layouts and limits.

The existing concurrent-reader case now clears the shared cache repeatedly
while four readers compare bytes against an independent snapshot. All **17
host reader cases pass**. The PS5 service suite adds a real file-replacement
test proving that cached reads retain their original handle until invalidation,
then reopen the new backing file. Other cases cover range bounds, invalid
operations, logging destinations, process binding and the game's actual CMIF
domain query format.

Console report `svc-runtime-1793832494-100.log` records **139 cases, zero
failures**. Payload SHA-256:
`a5f5a419ee919b142171ead9c217bafced636bd198917bd04e41c0d6ac3087d2`.
The native build and packaging/linkage checks pass. FSELF: 278,357,713 bytes,
SHA-256 `ba23692b37a80e741d55925c84a269ab278201424d5068e6d61d757e1d3b8bb4`.
Native ELF: 284,145,932 bytes, SHA-256
`8fcd346ce5488ed93b94715a2d2e486bf3f48fde225b405465a5ec8faf03cfb9`.
`game-renderer-fsp-install.json` records installation with the title stopped
and unmounted, a verified backup of the prior executable, and matching staged
and installed readback hashes. The owned FTP helper (PID 91) and completed
service-test process (PID 100) are gone; both owned payload slots were removed
and port 2121 is closed. Cleanup is recorded in
`game-renderer-wire-result-console.json`.
Report `game-native-1793832840-109.log` confirms the access-log query succeeds.
The title opens the selected USB RomFS twice, queries its size and issues
multiple successful storage reads. It then asks IApplicationFunctions for
command 210 and stops at that unported call with `0xbcbd5`. Dispatch reaches
the stop in 800 ms; native graphics remain initialized with zero draws and
presentations. The return Home is a controlled dependency exit, not evidence
of a native crash or a rendered game frame.

## Application events and launch channels

The supplied SDK's command-210 proxy at `0x75001bd8cc` calls the same
native-handle reply helper (`0x75000d4da4`) as the notification and health-warning
event getters. The proxy is present at relocated table entry `0x7500db82d0`.
Local, hash-verified NSO disassembly is retained in
`work/ps5/sdk-event210-evidence.txt`; this is Capstone/static evidence, not a
REA provider trace. SDK SHA-256 remains
`7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2`.
The event's purpose and producer remain unidentified. The implementation
returns a dedicated unsignaled readable event; it does not synthesize a signal.

The same batch ports friend-invitation and notification event/poll pairs
(140/141 and 150/151), plus the health-warning event (160). Each event is separate
and shared across repeated getters within one application state. Offline
channels have no producer and polls return `128-2` without an output object.
Only that exact result from commands 1, 141 and 151 is allowed through the
startup observer; malformed replies and unrelated errors still stop.

Launch-channel commands 1, 121 and 122 now support actual storage snapshots,
last-in-first-out retrieval and clearing through both session and domain IPC.
Storage passed to UnpopToUserChannel must be the port's typed applet storage;
later writes to the source cannot change queued bytes. The native queue is
bounded to 16 entries and 256 MiB total. No guest account has been selected,
so the preselected-user query returns no data. Reset isolates new sessions
from existing channels. These contracts follow the
[applet interface research](https://switchbrew.org/wiki/Applet_Manager_services#IApplicationFunctions)
and the inherited runtime's storage-channel implementation.

Basic application queries report the native frontend's fixed `en-US` locale,
the verified title display version, no guest recording support and no previous
program index. This does not enable save writes, recording, online invitations,
account selection or application switching.

Console report `svc-runtime-1793833203-119.log` records **146 cases, zero
failures**. The added cases exercise readable event handles through the SDK's
CMIF domain envelope, handle exhaustion, separate event state, exact no-data
results, storage snapshots/order/clear, moved session ownership, domain objects,
queue limits, reset isolation and bounded metadata outputs. Payload SHA-256:
`d442750fa38980885817be64fdeaa998f33b4253ee7ba21bf3bff623cc40131f`.
Native build, exception-linkage checks and packaging pass. FSELF:
278,379,713 bytes, SHA-256
`400ea821018be7686e9cac2dd73f4c99e1171d1185cf8f0d95a7f57e565ae9b9`.
Native ELF: 284,181,652 bytes, SHA-256
`e8def8f82ad3b6dfa0de93d393540ff79dd2880ff04bdefb4e0dfc3b48453e77`.
`game-renderer-channels-install.json` records a stopped/unmounted title check,
the prior executable backup, and matching complete staged/installed readbacks.
The transfer helper (PID 111) and completed test process (PID 119) are gone;
both owned payload slots were removed and port 2121 is closed, recorded in
`game-renderer-fsp-result-console.json`. Game files and shader caches were not
transferred or changed.
Report `game-native-1793833549-124.log` confirms command 210 succeeds. The title
continues through display/performance queries and opens `acc:u0`, which was not
registered in that build. The observer stops before any account command is
sent, after 197 SVC calls and 811 ms of guest dispatch. Heap allocation and
RomFS reads succeed. Renderer counters remain zero submits, draws and presents;
the "Waiting for first game frame" text is host status, not a game frame.
The app returns Home with the controlled dependency status 2.

## Offline account startup batch

The PS5 runtime now registers `acc:u0` and the read-only query subset of
`acc:u1`. One fixed local profile is open at launch. User count, existence,
lists, last-opened user, noninteractive selection, profile metadata and the
launch record use the same process-owned identity. Initialization commands
100/140/160 validate the PID-bearing eight-byte request and share initialization
state across sessions. On `acc:u1`, command 140 retains its different meaning,
ListQualifiedUsers. Registration, profile mutation and thumbnail writes remain
unported and do not report success.

The default UID is the existing runtime identity, first word `1`, second word
`0x6e78727475736572`, with nickname `Player`. Optional `[account] uid` (exactly
32 hexadecimal digits, nonzero) and `nickname` are snapshotted at registration.
Invalid IDs fall back to the default with a warning. Existing profile objects
retain their snapshot across reset, and handles cannot cross guest process
boundaries. This identity lives in the runtime; it does not create or alter a
PS5 shell account. Save-file import/mapping is not implemented by this batch.

The applet preselected-user channel now supplies the registered profile once
per launch using the 0x88-byte record consumed by
[libnx account initialization](https://github.com/switchbrew/libnx/blob/master/nx/source/services/acc.c).
It remains empty when no account service has been registered. ProfileBase is
0x38 bytes, UserData is 0x80 bytes with stock icon ID 1 at offset 4, and no JPEG
avatar bytes are supplied; those layouts follow
[the account structures](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/acc.h).
Guest output permissions and lengths are validated before copying.

The profile has no network-service account. Network availability, account-ID
and token-cache requests return `124-200` without an invented ID or token.
Async token/license requests return completed contexts with readable completion
events and the same registration-required result; the license kind is zero.
Only that exact result at the implemented account endpoints passes through the
startup observer. Unknown commands, malformed replies and unrelated failures
still stop. Requesting a network-qualified user returns an empty UID, and the
fixed single-profile frontend reports account switching locked.

The actual supplied SDK's application proxies at `0x7500190bfc` (140) and
`0x7500190c2c` (160) use `0x75000c76c4`, which marshals an eight-byte argument
and sends the PID. This is static evidence from the private generated SDK
source, not proof that the game's next runtime command will be either one.

Console report `svc-runtime-1793833969-131.log` records **156 cases, zero
failures**. The ten added cases cover domain IPC, receive-list buffers,
preselection storage, PID/init state, identity consistency, profile byte layout,
buffer boundaries, process isolation, reset snapshots, offline results,
completion-event ownership and handle exhaustion. Payload: 4,405,936 bytes,
SHA-256 `cd12f8d8a95a91dd6e4d02e4ac658bb294e772917999a42eb9a5c03718b4652e`.
This is console service validation. The subsequent native run below confirms
account initialization and launch-record consumption; other account operations
remain unverified in actual game startup.

The local native build, exception-linkage checks and packaging pass. FSELF:
278,441,793 bytes, SHA-256
`02feb52e59d376e6e43484cc8c4a3d57b565e67d1e0b3515454e4b604e9135c2`.
Native ELF: 284,240,116 bytes, SHA-256
`b2287e0bbd2ea437bbdcb76c2bd33cb7e59a9fbd43900e23dfa8675de3c433de`.
The private module stamp, RADV/platform libraries and startup shader package
are unchanged. Source and build instructions remain in Git; compiled game
code and console reports remain in the ignored local work directory.
`game-renderer-account-install.json` records a stopped/unmounted title check,
the previous executable backup, and complete matching staged/installed SHA-256
readbacks. The owned transfer helper (PID 126) and service test (PID 131) are
gone, their two temporary payload slots are removed, and port 2121 is closed.
Cleanup is recorded in `game-renderer-channels-result-console.json`. No game
files or shader caches were transferred or changed.

Report `game-native-1793835778-92.log` confirms `acc:u0` command 160 succeeds.
The title reads the preselected-user storage through `IStorageAccessor`, closes
the domain objects, and queries the account-switch policy. Startup then stops
at `IAddOnContentManager` command 50 with `0x4201` (not implemented). The trace
captures a plain CMIF request with 12 trailing/padding bytes, all zero; that
does not establish a 12-byte argument. The no-input/output contract comes from
the service implementation cited below. Dispatch reaches 210 SVC calls in
813 ms. Renderer counters remain zero submits, draws and presents; the text
"Waiting for first game frame" remains a host status message. Exit status 2
is the controlled dependency stop, not a native crash.

## Add-on status, notification preference and locale batch

`IAddOnContentManager` command 50 now checks the port's empty add-on inventory:
there are no installed/mounted add-ons to lose, so the status is successful
with no return data or handle. The contract matches the local Eden
`core/hle/service/aoc/addon_content_manager.cpp` implementation and the
[AOC command table](https://switchbrew.org/wiki/NS_services#aoc:u).
Foreign resources are rejected and the root service is bound to its guest
process. This does not implement preparation, mounting, authorization or
purchasing; those operations still fail explicitly. Events remain unsignaled.

The batch also ports `ISelfController` command 100 as a stored boolean
screenshot-notification preference. Its one-byte request is validated and the
preference resets with application state. This follows the inherited applet
command table and Eden's `am/service/self_controller.cpp`. No screenshot
producer or capture facility is enabled by subscribing.

Read-only locale prerequisites are now available through `set` and the
`ns:am2`/`ns:ro` getter (command 7989). Settings exposes language code, legacy
and modern language lists/counts, language-index conversion, USA region,
non-kiosk status and a bounded `Pokopia PS5` runtime nickname. The applet and
settings share the existing fixed `en-US` frontend locale. These are runtime
preferences, not queries of or changes to the PS5 shell's settings.

The NS child ports commands 1/2/3: supported-language selection and conversion
between application indices and eight-byte codes. Its 16 application indices
are distinct from the 18 settings indices; Chinese legacy aliases normalize
to their corresponding modern codes. Selection prefers American English,
then British English and the American-English fallback order, with Brazilian
Portuguese adjacent to Portuguese. A mask containing no supported languages
returns NS `16-300`. The language structures/order and error code follow
Eden's `ns/language.{h,cpp}` and `ns/ns_results.h`; settings request layouts
follow [libnx set.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/set.c).
The frontend still has no locale picker. Administrative settings, firmware
data, title-control metadata, store/rights and installation interfaces remain
unported. Buffer bounds, guest permissions and process identity are checked.

The notification and locale additions are proactive startup prerequisites;
this PS5 report did not reach them. They are not claimed as the next observed
dependency or as evidence of gameplay. No new blanket error bypass was added.

Console report `svc-runtime-1793836111-102.log` records **163 cases, zero
failures**. The seven added cases exercise the observed plain CMIF status
query, resource/process rejection, exact one-byte notification domain input,
settings receive-list/map-alias outputs, all language mappings, invalid masks,
truncated/readonly/overflowing buffers, and cross-service locale consistency.
The service payload is 4,579,288 bytes, SHA-256
`c2a42f1a863a35ff51796b09c244b4d1cc26b46057968f0cb5d1689dca1bd83b`.

The local native build, exception-linkage checks and packaging pass. FSELF:
278,478,497 bytes, SHA-256
`d156d9ea4281c0e9115a5650e6413e23d5e5adfba3daebfffa6497de53e08ea4`.
Native ELF: 284,279,604 bytes, SHA-256
`832c6be4c94f77bc4b71cc0d97c391dfb63436621ce166c029985cde97c81955`.
Only `eboot.bin` differs from the prior title package. The private module stamp,
RADV/platform libraries, native libc, provenance and startup shaders are unchanged.
The first installation attempt lost its FTP connection while reading the old
executable backup, before upload or rename. The 228,943,192-byte incomplete
local backup was hashed in `game-renderer-locale-deployment-status.json` and
removed. Ports 8084, 10101 and 2121 then timed out. That attempt did not install
the candidate or change the previous account executable.

After console services returned, the prior helper was absent and its two owned
payload slots were removed. The replacement completed with the title stopped
and unmounted, a verified backup of the prior account executable, and full
matching staged/final SHA-256 readbacks. Installation is recorded in
`game-renderer-locale-retry-install.json`. The new helper (PID 103) and its
temporary payload slot were removed; port 2121 is closed. The two cleanup
receipts are `game-renderer-account-result-console.json` and
`game-renderer-locale-install-console.json`. The combined build is installed;
game startup beyond the add-on status query remains unverified.

## Worker stack allocation

Report `game-native-1793838222-107.log` passes the add-on status query and the
offline account/profile calls. The next failure is SVC `09` (StartThread),
handle `0x10019`, returning `0xce01` (OutOfResource). Two earlier guest workers
started successfully. The game then sets its termination result and calls
Break (`26`); the observer stops with status 2. At 225 SVC calls, the report
still records zero submissions, draws and presents. The loading message is
not evidence that a game frame was generated.

The native link routes thread creation through the pinned graphics platform's
stack wrapper. In SDK revision `b83202be73e930050e00de0f1b4d9ec46c0391bf`,
[`platform/src/threads.c`](https://github.com/mihawk-99/PS5_PayloadSDK/blob/b83202be73e930050e00de0f1b4d9ec46c0391bf/platform/src/threads.c)
uses direct memory only for default/small stacks; an explicit size at least
2 MiB takes the plain libkernel path. Our 64 MiB guest-worker stacks therefore
used the limited flexible-memory pool. This is a source-supported explanation
for the resource failure, not a captured native errno from that earlier run.

The PS5 host-worker backend now owns direct-memory backing for its requested
stack size, rounded to a host page, and supplies the address to pthreads.
An unbacked lower guard page remains inaccessible. It releases the reservation
and backing after a successful ordinary or timed join, including all TLS
destruction, or if creation fails. Timeouts and entry-completion observations
retain the stack. The guest default remains 64 MiB; this is an allocation-path
change, not a stack-size reduction or a native-memory-capacity claim.

Failures now record their stage and native errno; kernel logs record worker
handles, entry points and requested sizes. A fatal four-byte SDK Break result
is captured only when its guest buffer is readable. Break remains fatal;
there is no new error-success bypass.

The host platform CTest passes. Console component validation:

| Report | Cases | Failures |
| --- | ---: | ---: |
| `platform-1793839390-148.log` | 78 | 0 |
| `kernel-runtime-1793839473-151.log` | 22 | 0 |
| `svc-runtime-1793839423-150.log` | 163 | 0 |

The new concurrency case runs two rounds of eight workers, verifies actual
stack addresses/sizes, executes with 1 MiB of local stack data, checks writable
stack/inaccessible guard protections, and checks unmapping after both join
paths. Payloads report a zero direct-memory limit and cannot support the
native-app-sized test: the initial free-range query failed, and the subsequent
64 MiB-per-worker attempt hit allocation error `80020023`. Those reports remain
`platform-1793838747-128.log` and `platform-1793838844-132.log`. The final test
uses eight 8 MiB stacks in the payload sandbox, or eight 64 MiB stacks when
run as a native app with a reported memory budget. Only the payload variant
has been executed. Existing cases also cover explicit exit, TLS destructors,
unstarted cancellation, timed join retry and guest-thread lifecycle.

The first kernel run lacked its original TLS fixture and failed one case.
`runtime/tests/data/fixture_aarch64.elf` was restored to the reusable console
test-data directory and readback-verified with SHA-256
`9d4b3055134b2035d01c679713e4214a82c2ff85e7082b4838d7a91360cde27a`;
the unchanged kernel payload then passed all 22 cases. No game files were used
for these component tests. Payload identities, in table order:

- `2fa65f516895ad34f86238d1dbe4b5f631e7868ac84f4be34a2175306681fe16`
- `a82c8b87454c0dfb12ac94d18fc2663eaa5630eaf78280bb7ed93f2b21fb62ad`
- `c9e5e8c0cd7f7af3c5fe02e31b443535cf85652148d1a5cac7754ced5bf75315`

The native build and packaging pass. FSELF: 278,480,097 bytes, SHA-256
`c2ec629b879be7e910376070e0bae2d887a891217491dbf528c692c16071af91`.
Native ELF: 284,279,676 bytes, SHA-256
`d7d17583882000b41ce28e7b4c617ad5a6a436e7c401c1bd8b693174454df3b2`.
Only the executable differs from the preceding locale package. Actual game
startup beyond the failing worker and the 64 MiB native stack path still need
the next native launch.

`game-renderer-thread-stacks-install.json` records replacement with the title
stopped/unmounted, a verified backup, and matching staged/final full-file SHA-256
readbacks. The installed hash is `c2ec629b879be7e910376070e0bae2d887a891217491dbf528c692c16071af91`.
The transfer helper (PID 122) and all seven owned payload slots were removed;
port 2121 is closed. The receipt is `game-renderer-thread-stacks-console.json`.
The previous helper had already disappeared in the console restart and its
remaining slot was also removed. No app launch was initiated during this batch.

## First presentation and startup bounds

Report `game-native-1793839656-152.log` starts all 23 guest workers with their
64 MiB direct-memory stacks. This exercises the previous allocation fix in
the real native app. The renderer reports seven submissions, one draw and one
presentation. The user saw a black screen, then Home; correct game imagery,
a menu and gameplay remain unverified.

This return is the diagnostic's old 10,000-SVC ceiling, reached after 7,150 ms
of guest execution. Three workers reached it together and duplicated the final
report. The native renderer build now relies on its independent 180-second
watchdog instead of that count ceiling. One worker owns final reporting and
shell exit; concurrent stop requests wait for process closure. Other probe
builds retain their count ceilings and fatal dependencies still stop startup.

The observer also treated the preserved argument registers from SleepThread
and SignalProcessWideKey as error Results. The report contains 5,090 sleeps
and 1,426 signals, with substantial observer time spent on those log writes.
These void SVCs now log the first call without logging every nonzero argument
as an error. Their runtime behavior is unchanged.

The report requests 461 missing shader hashes. All exist in the verified
private corpus. Selecting their first-source entries yields 480 shaders from
16 entries: 397 fragment and 83 vertex. All translate successfully and pass
Vulkan 1.3 validation. Merging them with the original 153 shaders yields 633
validated blocks, preserving the old SPIR-V bytes and each resource-binding
row. This covers every observed miss, not the entire game. Unimplemented NVN
name lookups in the report do not establish that those functions were called.

Four host cache checks pass: observed-entry selection without unrelated shared
occurrences, source-content corruption and unknown-key rejection, readback and
filename validation, and backup restoration after publication failure. The
native build, exception linkage and FSELF integrity checks also pass. FSELF:
278,469,665 bytes, SHA-256
`ce0c09af765fc0be592900dd0c680a4cfe37e1d265d5313b80e652c7789a557a`.
Native ELF: 284,279,676 bytes, SHA-256
`2a36b17265f2d2d784688f97d4782c1c077c185e3baa7aa4fdab147695b4249d`.
Only the executable and separately installed shader cache change; the selected
game folder, saves, module inputs, RADV/platform libraries and native libc are
unchanged. The combined candidate still requires a native startup observation.

The prepared cache contains 105,049,465 bytes before its file manifest; that
manifest has SHA-256
`55c7eb6fad4375ca480efcc4c4a0547f7397f2a4ba4b11842a46c91a878cd2f5`.
At preparation completion, the console had restarted and ShadowMountPlus was
offline, so no replacement was attempted. The log-collection helper was gone
and its remaining owned payload slot was removed. The candidate and pending
installation state are recorded in `game-renderer-first-frame-deployment-status.json`.

After services returned, both replacements completed with the title stopped
and unmounted. `game-renderer-first-frame-shaders-install.json` records all
636 cache files, matching staged/final SHA-256 readbacks, and the preserved
previous-cache directory. `game-renderer-first-frame-install.json` records the
verified old executable backup and matching staged/final readbacks of the new
FSELF. The installer now changes directory before MLSD because this PS5 helper
ignores MLSD's path argument; the first preflight stopped before any write.
The four host cache checks pass with this behavior covered.

The transfer helper (PID 105) and its one owned payload slot were removed;
port 2121 is closed. Cleanup is recorded in `game-renderer-first-frame-console.json`.
The combined build is installed, but it was not launched automatically and
its game imagery remains unverified.

## Active shader loading and wait-loop profiling

Report `game-native-1793842568-92.log` recognizes all 633 installed records and
continues loading for the full three-minute window. The first new missing
shader appears around log time 47.7 seconds, and new shader programs and
filesystem reads continue until the watchdog expires at 180,017 ms of guest
execution. The final counts remain seven submissions, one draw and one
presentation, with no raised guest exceptions. The user saw black throughout
and then Home. This is active startup loading with incomplete shader coverage,
not evidence of a rendered game image or a crash at the final stop.

All 571 new missing hashes are in `0x05d525ae.fdata`. The next selection covers
that entire source archive: 3,354 unique shaders (3,166 fragment, 188 vertex).
All translate with status `ok` and pass Vulkan 1.3 validation. The combined
cache contains 3,627 shaders, retaining every old shader and its resource-binding
metadata. Coverage is still incomplete outside the selected archives.

The final profile records 24,001,412 SleepThread calls. The reported caller
`main+0x1ff69b8` is a job-wait loop calling the SDK's YieldThread import. The
full observer accumulated 43,633 ms around those handlers across workers;
that is neither a CPU percentage nor a measurement of shader loading itself.
No scheduling behavior is changed on that evidence alone. The native observer
now times one in 1,024 SleepThread calls while preserving exact call counts.
Reports label these as sampled durations, include the timed-call count and
last sampled argument, and do not extrapolate totals.

The native shader cache now measures complete lookups, including hashing,
file access and reflection. Every ten seconds the watchdog reports loaded and
missing shader counts, completed lookups, summed lookup time and the longest
lookup, alongside draw/present counters. This will distinguish time spent in
the host shader path from other game startup work. The startup window is now
600 seconds so another active archive load is not automatically cut at three
minutes. Fatal dependencies still stop immediately. The memory profile,
controller behavior, original game files and saves are unchanged.

The four host cache checks pass, including complete-source selection and
rejection of source expansion without an observed log. The native build,
exception linkage, ten-minute manifest and FSELF integrity checks pass.
FSELF: 278,475,169 bytes, SHA-256
`1ef938bac821936dc5ee9531dc9d55cd2acb2e06790c078e5626ea956296dc7e`.
Native ELF: 284,267,452 bytes, SHA-256
`bf466e0f45e583e0b47f27a4b78f3f59fb7acdad7ba4368110c837f9d2296daa`.
Console services became unreachable after report collection, initially
preventing installation. The candidate's startup behavior remains unverified.
The cache is 839,314,530 bytes before its file manifest; that manifest's SHA-256
is `c0cc765711f7e943a0cbc709b76296ea41dcd72be317fad8cdeb12d9a52d9983`.
Package identities and deployment status are recorded in
`game-renderer-startup-bank-deployment-status.json`.

After services returned, the cache and executable were installed with the
owned title stopped and unmounted. All 3,630 cache files passed complete
staged and final SHA-256 readbacks; the previous cache directory is preserved.
The executable also passed both full readbacks, with its predecessor backed
up locally. Receipts are `game-renderer-startup-bank-shaders-install.json` and
`game-renderer-startup-bank-install.json`. Future cache installations report
readback progress every 250 files as well as upload progress.

The earlier collection helper was gone, and its PID 93 now belonged to
Payload Manager. Cleanup preserved that process and removed only the recorded
transfer payload slot after confirming the FTP port was closed. Three host
checks cover reused PIDs, unrelated FTP servers and pending diagnostics.
The prior receipt is `game-renderer-first-frame-result-console.json`.
The new transfer helper (PID 101) and its one owned payload slot were also
removed, and port 2121 is closed; its receipt is
`game-renderer-startup-bank-console.json`. No app launch was initiated.

## Shader residency and allocation failure

Report `game-native-1793846622-88.log` ends shortly after 400,123 ms of guest
execution with a system internal-memory warning. This was a native crash before
the 600-second watchdog, not a controlled return. The kernel recorded SIGILL;
symbolication against the matching unconverted executable places the fault at
the allocation runtime's `operator new` failure trap. Its callers are the
shader-cache file reader, `ShaderCache::load/find`, and program registration.
The last progress record had 2,936 loaded shaders, zero missing requests,
3,672 lookups taking a summed 10,161 ms, seven submissions, one draw and one
presentation. There was still no verified game image. The warning does not by
itself establish a thread leak or identify the exhausted allocator pool.

The PS5 shader cache now keeps reflection, controls and copied constants in
stable `ShaderInfo` objects, then releases the raw SPIR-V after registration.
Pipeline creation and variants acquire immutable handles from a 128 MiB LRU
word store. Eviction drops the cache's ownership, while active readers retain
theirs. Synthesized shaders retain their in-memory words. The limit covers
idle raw shader words, not metadata, Vulkan modules, active-reader pins or the
entire graphics working set. The desktop preload queue is disabled on PS5.

Shader reads now allocate the final word vector directly, avoiding a growing
string stream and copied string. Files with an invalid header, unaligned size
or size over 64 MiB are rejected before reflection. The startup report includes
word residency/read counts, platform-heap mapped/peak bytes, segment count,
libc fallbacks and the largest available direct-memory block. This measures
whether another pool or allocation pattern remains a problem.

The standalone synthetic reader/eviction suite passes **4,116 checks** on the
host and PS5, including 4,096 repeated reads under a two-entry budget, pinned
handle survival, eight concurrent readers, oversized entries and malformed
files. Console report: `shader-words-1793847886-94.log`. It removes only its own
generated test directory. The payload test does not establish native game
startup or pipeline correctness; the rebuilt native app still needs a run.

The host checks require no game files:

```sh
cmake -S ps5/tests/shader_words_suite -B work/ps5/shader-words-host -G Ninja
cmake --build work/ps5/shader-words-host
ctest --test-dir work/ps5/shader-words-host --output-on-failure
```

For a PS5 payload build, use the pinned SDK toolchain as with the other suites.
Private shader files and recompiled executables remain excluded from public
distribution. The release still requires on-console generation from the
user's files; this change bounds runtime residency of those local outputs.

The native app rebuilt, passed exception-linkage and FSELF checks, and was
installed with the owned title stopped and unmounted. Its complete staged and
final readbacks match SHA-256
`2b8bdc96330d80c147aed88b32a72a70c6c44dbf77fa6db849b45c8a4badfe45`
(278,476,721 bytes). The corresponding converted ELF is 284,267,732 bytes,
SHA-256 `4a7eabdf9ce7a068d94bd0cc875f28ade17c75cb75b6b0f84785036fd103dc4e`.
The previous executable is backed up locally; the installed shader files are
unchanged. Receipt: `game-renderer-shader-memory-install.json`. The diagnostic
payload exited, the owned transfer helper and payload slots were removed,
and port 2121 was verified closed. No native app launch was initiated.
This establishes installation and synthetic cache behavior, not a fix verified
against the game's startup or a rendered menu.

The subsequent monitored run, `game-native-1793848371-95.log`, passed the old
allocation-failure point and reached 3,596 loaded shaders. The host heap stayed
at 201,326,592 mapped/peak bytes with zero libc fallbacks; the shader-word store
held 12,236 bytes in two entries. These are host allocator/cache measurements,
not the game's total memory usage. The last direct-memory query reported a
168,263,680-byte largest free block after additional guest workers started.

At 557,740 ms the diagnostic stopped on `SDK_STOP unported service=aud:u`, then
returned status 2 through the normal native exit path. It did not reach the
600-second deadline or repeat the observed allocation crash. It also logged
139 distinct missing shaders, with seven submissions, one draw and one present
still recorded. No game menu or gameplay was established. The missing audio
service is the immediate stop; the shader misses are a separate rendering gap.

The monitoring helper and its owned payload slot were removed and port 2121
was verified closed. The full report, kernel log and time series remain private
under `work/ps5`; no app relaunch or executable replacement was performed.
The source review for the next batch is [UPSTREAM_REVIEW.md](UPSTREAM_REVIEW.md).

## Interpreting a run

Reports are `/data/pokopia-recomp/tests/game-native-<time>-<pid>.log`.
The current 900-second deadline starts at guest dispatch, after folder selection and
input checks. `GAME_GRAPHICS_BRIDGE` confirms host routing is installed;
`GAME_GRAPHICS_READY` confirms the title called it. `GAME_NVN_PRESENT` records
presentation, while `GAME_RENDER_REPORT` records submissions and draws. Those
counters alone do not establish correct imagery or playable input.

A missing dependency or expired deadline returns Home with status 2 and
`GAME_TRACE_END`. That remains an incomplete run, not a pass. No game frame or
menu has been verified for this combined build. The prior purple-screen run
qualified synthetic NVN replay and texture conversion only.
