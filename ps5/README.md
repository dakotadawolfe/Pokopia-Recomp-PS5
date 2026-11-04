# Native Pokopia for PS5

The main goal is a native PS5 application using the existing static recompilation
and the user's decrypted Pokopia files. Development and builds run locally.
The initial upstream revision is `2c29515b23a6d78d967775fd7e9f24f50402cca7`.

## Product scope

- Load decrypted ExeFS and RomFS from `C:/Games/pokopia` during local preparation.
  An explicit input path can point to an existing dump elsewhere. Do not request
  keys or firmware; this application consumes extracted game files.
- Produce a native PS5 application launched through ShadowMountPlus, using
  the existing homebrew SDK, RADV and native presentation work where appropriate.
  Deliver the complete title folder with `eboot.bin`, `sce_sys` metadata and
  required runtime modules. Keep the native application ELF and symbols for
  debugging. Payload Manager ELFs are development diagnostics; their successful
  execution does not establish a working ShadowMountPlus application launch.
- Target steady 60 real frames per second, with frame generation off. Establish
  one fixed graphics profile against the base PS5 hardware, then validate it on
  other PS5 models. Do not expose resolution, upscaling, GPU accuracy, performance
  presets or other graphics tuning in the product.
- Provide basic controller mapping, stick sensitivity/deadzones, invert look
  and vibration controls, with persisted preferences. DualSense is the initial
  controller target; other controller support requires device verification.
- Provide audio volume controls, reliable save/load, safe save import/export,
  clean exit, clear missing-file errors and useful crash logs. Retain applicable
  game language and accessibility controls.
- Keep original game files and existing saves intact. Keep all game-derived
  output and private input manifests outside Git.

60 FPS is the performance target, not a measured result. The port is not complete
until boot, rendering, controller input, audio, saves and sustained gameplay have
been verified on console. Cross-model support also requires representative
hardware testing; a base-console run does not establish PS5 Pro behavior.


## Intended installation layout

Local preparation consumes the user's decrypted dump. The final native
application will contain the recompiled x86-64 game code and platform runtime,
packaged as `eboot.bin` and any required modules for ShadowMountPlus. Game assets
remain separate under `/data/pokopia-recomp/game/`; they are not embedded in the
ELF. The current loader requires the decrypted ExeFS images for their original
mapped contents and layout as well as the separately compiled code. RomFS asset
staging and loading are still to be implemented.

The intended default is internal PS5 storage, with USB or FTP used for transfer.
After installation, playing should not require the PC, keys or firmware. Loading
the full game directly from USB is a separate, unverified option. Save data and
controller/audio preferences will live in writable application-data directories,
separate from game inputs and the app package.

## Input validation

From the repository root, with Python 3.10 or newer:

```powershell
python ps5/validate_input.py
```

The default source is `C:/Games/pokopia`. It supports either a directory with
`exefs` and `romfs`, or an extracted tree with `base` and `updates/<version>`.
Use `--input PATH` for another location. From WSL, pass the `/mnt/c/...` path.

The validator matches every executable module and `main.npdm` against upstream's
supported hashes, checks the NSO build ID, and records the available RomFS layers.
It writes only `work/ps5/input-manifest.json`; it does not copy or alter the dump.
That ignored manifest records paths for later preparation. RomFS file presence
and size are checked, but asset completeness is not claimed from those checks.
Any future asset staging must apply recorded layers in order, with later layers
overriding earlier ones.

Validated locally on 2026-11-03:

- `main`, `sdk`, `subsdk0`, `rtld` and `main.npdm` match upstream's supported hashes.
- Main build ID: `76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E`.
- The existing dump has the supported executable under update `655360`, with
  a RomFS directory containing about 7.25 GB of files.
- The requested default directory is not currently present; validation used the
  existing dump through `--input`. Machine-specific paths stay in the ignored
  manifest.

## Bring-up milestones

1. Validate the decrypted inputs and establish reproducible local preparation.
   Module hashes are verified and all four game modules have been lifted.
   Asset preparation and full game linking remain.
2. Port memory reservations, aliases, protection and fault handling. Coordinate
   guest addresses with the PS5 GPU allocator before combining the runtimes.
3. Port thread, synchronization, filesystem and exception services, and exercise
   their behavior with runtime tests.
4. Link the lifted game and reach its startup using the PS5 runtime.
5. Integrate RADV/VideoOut, shader preparation, audio and controllers.
6. Validate saves and sustained gameplay, then select the fixed graphics profile
   from console frame-time measurements.

The earlier compiler proof translated an original 34-instruction ARM fixture,
matched its results on a PC and linked it into a PS5 ELF. It did not execute
Pokopia or run on console. The detailed
[portability audit](https://github.com/dakotadawolfe/EglantinePS5/blob/44e3f3e/docs/POKOPIA_RECOMP_PS5.md)
and reproducible test are retained in the existing emulator project.

## Local native build

Install the PS5 payload SDK, LLVM 18, CMake, Ninja and a Python environment with
`recomp/requirements.txt`. Set `PS5_PAYLOAD_SDK` to the SDK root and `NX_PYTHON`
to that Python executable. On Linux/WSL, from this checkout:

```sh
export LLVM_CONFIG=/usr/bin/llvm-config-18
bash ps5/build_modules.sh /path/to/decrypted/Pokopia /path/to/private/build/modules
bash ps5/check_platform.sh /path/to/private/build
```

`BUILD_JOBS` limits compiler concurrency and defaults to four. The build writes
all generated game code and archives under the supplied private build directory.
It reads the original dump without copying or changing it. The module bases,
hook sites, floating-point contraction setting and generated-code optimization
level match upstream. The memory API preserves that layout; full loader
integration remains to be tested.
After compilation, `check_modules.py` inspects the ABI symbol inside each game
archive, checks the generated function/unit counts and rejects emitted
`guest_unimpl` calls. It writes hashes and coverage counts to
`module-verification.json` inside the private build directory. This check does
not execute the game or establish complete function discovery.

The component target produces `libpkp_main.a`, `libpkp_sdk.a`,
`libpkp_subsdk0.a`, `libpkp_rtld.a`, `libnxrt_ps5_platform.a` and
`libnxrt_ps5_core.a`, `libnxrt_ps5_kernel.a` and `libnxrt_ps5_svc.a`.
These are link inputs, not a finished game executable.
The platform library contains synchronization, logging, configuration, the
gauge registry and the PS5 guest-memory API, including sparse fixed regions,
shared heaps and aliases.
The core library contains dispatch, the generated-code ABI,
module registration, hooks, NSO loading and linking. The kernel component adds
guest thread objects, handles, TLS, timing and synchronization queues. The SVC
component connects 37 handlers to the generated-code ABI: thread, event,
synchronization, timing, debug strings, boot memory, process information and
three synchronous IPC calls. The service manager, log manager and initial
application-proxy interfaces are connected.
Remaining memory services, other services, guest exceptions, write tracking and rendering still need integration
before the core can run the game.
The optional Windows frame recorder and stack sampler are disabled in the PS5
core build; ordinary HLE call logging remains available.

The platform check runs 68 cases on the host: 17 upstream cases for mutexes,
condition variables, events and semaphores, 23 upstream cases for logging and
configuration, eight memory cases, nine subpage cases and eleven host-worker cases. The PS5 build runs
77 cases, including two additional direct-memory ownership/release tests and
seven sparse guest-region cases. It writes a report
to `/data/pokopia-recomp/tests/platform-<time>-<pid>.log`; individual temporary
test files are removed by the tests. Windows retains its native synchronization
primitives; the PS5 backend uses the target C++ threading runtime and a monotonic
clock. Host success does not prove the PS5 backend's runtime behavior; collect
the console report for the exact uploaded ELF and verify the process exits.

The lifted output currently contains 200,586 function entries across 280
translation units, with no emitted `guest_unimpl` calls in those units. This
checks translation coverage of the discovered functions, not execution or
complete discovery of every possible runtime code path.

On 2026-11-03 all four game archives and both runtime component archives built
locally with the PS5 SDK. The current 57 host platform cases passed. The PS5 suite was
uploaded, readback-verified and executed: **66 cases, zero failures**, followed
by a clean process exit. Test ELF SHA-256:
`3cb93fc2c8c80e61960d5ff159e8e80c69aea44d6a15b9702de05144320682e3`.
The saved report is `platform-1793766832-109.log` in the private `work/ps5/`
directory. This establishes platform behavior, not game boot or performance.

## Native memory foundation

`nxrt/host_vm.h` owns anonymous/shared mappings and rejects unaligned,
out-of-bounds or overflowing operations. Exact-address requests never replace
an occupied mapping. Host shared objects use POSIX backing; PS5 shared objects
use zeroed direct memory because `shm_open(SHM_ANON)` failed in the native app
despite passing in a payload. Views retain their backing independently of the
object. These primitives serve small allocations; large guest regions use the
sparse owners built on `nxrt/ps5_vm.h`.

The PS5 implementation reserves virtual ranges with
`sceKernelReserveVirtualRange` and attaches direct-memory allocations only to
owned, previously unbacked subranges. Every view retains its backing until the
view is unmapped. Discarding a subrange atomically restores its reservation;
split views and independent aliases retain their backing. This avoids opening
an address-space hole between unmapping and reserving. Protection changes require complete backing and host-page
alignment. The guest heap must initialize new physical pages before exposing
them to game code.

Hardware findings that determine the implementation:

- Host protection pages are 16 KiB. A successful `mprotect` for 4 KiB also
  protected the neighboring guest page. The low-level wrappers reject that
  request instead of silently rounding it. The subpage policy below preserves
  guest permissions; integration with the guest heap and write tracker remains.
- The second ordinary 4 GiB anonymous reservation returned `ENOMEM`. Native
  reservations successfully held the 4 GiB stack region, 4 GiB kernel region
  and 12 GiB heap region simultaneously at their original guest addresses.
- The full-layout test backed only three 16 KiB pages, including two distant
  heap pages and an alias. This verifies address space, not a 12 GiB physical
  memory budget or GPU compatibility.
- `sceKernelGetDirectMemorySize` reported zero in this payload context while
  allocations using that returned bound succeeded. The wrapper passes the
  value through and checks the allocation result rather than rejecting zero.
- The diagnostic alias-enable call returned an error, but both direct-memory
  mappings shared writes correctly. The production wrapper does not depend
  on that call.

`ps5_memory_capabilities` is a separate bounded diagnostic. It records host
page behavior and native reservation/alias results under
`/data/pokopia-recomp/tests/memory-<time>-<pid>.log`. The native capability run
reported zero failures; the final platform suite exercises the production
wrappers rather than substitutes. The initial capability results came from a
payload; the native app now also passes the production memory-wrapper cases.

`nxrt/subpage.h` records permissions at 4 KiB granularity and applies their
union to each 16 KiB host page. A 32 MiB flag table covers the guest address
space. Generated scalar, vector, atomic and cache-zero memory operations check
that table; only pages with mixed permissions enter the locked permission
check. Cross-page and long accesses check the entire range before writing.
Permission updates mark affected host pages first, coalesce native protection
runs, and restore earlier runs if a later native operation fails.

PS5 builds use ABI 4, which has the same CPU/module layouts as ABI 3 but requires
these memory helpers. Runtime registration rejects a stale ABI 3 game archive.
Windows keeps ABI 3 and its existing hardware protection path. The Windows
source list explicitly excludes the PS5/POSIX memory implementation files.

The platform build uses host Clang 18 and the configured recompiler Python to
assemble and lift an original eight-function, 17-instruction ARM fixture. The
console suite checks its memory/register results and permission faults for
ordered loads/stores, exclusive operations, CAS, swap, vector stores and cache
zeroing. Other cases cover neighboring 4 KiB pages, boundary-crossing writes,
rollback, larger accesses and concurrent readers. These tests need no game dump.

`nxrt/guest_region.h` combines the native allocator and subpage policy for fixed
module, stack and kernel regions. It allocates backing in 2 MiB blocks, clears
new memory through a private alias before exposing the primary view, and tracks
committed guest pages separately from their permissions. Recommitting a live
page preserves its contents. Decommitting clears that guest page, preserves
neighbors, and reclaims blocks with no remaining committed pages. The console
tests verified these behaviors, including a 4 GiB reservation with only 4 MiB
of backing for two distant pages. This owner deliberately does not implement
heap aliases, whose decommit semantics must retain shared contents.

`nxrt/shared_heap.h` adds sparse section backing with independent primary,
guest-alias and private import views. New backing is cleared before exposure;
primary decommit retains the section contents and existing aliases. Views can
precede backing allocation, share later commits and change permissions
independently. Mapping/protection failures roll back earlier changes. Private
views use the same 4 KiB policy without entering the guest-address registry.

The separate `ps5_shared_heap_tests` payload passed all 13 cases on the console
on 2026-11-03, including injected native mapping/protection failures, alias
lifetimes, sparse commits, preserved contents and clean recreation. ELF SHA-256:
`d3f5fcbda4581acfe58c89cc8894965c77f916cf8b2722cd72b4169edaa564a0`.
The 57 host platform cases also passed. Those 13 owner-level cases ran as a
payload; the public heap API has since been exercised in the native suite below.

`runtime/src/mem/memory_ps5.cpp` now connects these owners to the public memory
API: initialization, fixed reservations, kernel allocations, primary/alias/private
heap views, protection and VM metadata. Cross-reservation protection preflights
all pages and restores earlier changes if a later operation fails. The control
operations are serialized, and failed initialization preserves foreign mappings.

The 30 memory API cases include 26 upstream cases and four PS5 cases for rollback,
concurrent allocations and bounds. In a payload, 29 passed and the final 256 MiB
commit failed with a native allocation error. The same full-size case passed in
the native application, together with all other API cases. This establishes the
tested allocation size in that context, not a full game memory budget.

Runtime/HLE buffer accesses still need permission checks and write tracking.
Guard overhead has not been measured in Pokopia. The loader propagates a failed
protection operation instead of marking a module linked; the payload loader
suite below now exercises that path against the native memory backend.

Next: validate the game's NSO images against the loader, port write tracking,
and partition the RADV
address arena before initializing graphics. The existing RADV arena spans
`0x4000000000` through `0x8000000000` and overlaps the lifted game layout.
Do not combine the allocators until their ranges have been separated and tested.

## ShadowMountPlus runtime app

The runtime suite also builds as the native title **Pokopia Runtime Tests**,
`PPSA98731`. This is an application-process check before game integration. It
contains original ARM fixtures and runtime tests. Optional loader and SDK probes
also include recompiled Pokopia modules and require the user's decrypted images.
It uses the native application CRT, kernel provider, C++ libraries and companion
`libc.prx`, rather than the payload CRT. Its current 226 cases comprise 77 platform
cases, six thread-lifetime/exception cases, seven application-heap cases, 30 memory
API cases, 16 loader cases, 22 guest-kernel cases and 68 SVC cases. The optional
decrypted game-image check brings the total to 227; either loader probe makes 228.

The app supplies C++ thread-local destructor registration and wraps statically
linked `pthread_create` calls. Worker cleanup handlers drain C++ destructors before
the SDK frees emulated TLS storage, including normal return, `pthread_exit` and
cancellation. Destructors may register further destructors. Modules must remain
loaded until process exit; threads created inside external PRXs are outside this
wrapper and need separate integration checks. The six focused cases passed both
on the host and in a PS5 payload before the native app run.

Use the pinned public native app toolchain, prepared locally:

```sh
git clone https://github.com/drakmor/ps5-native-app-boilerplate.git /path/to/native-tools
git -C /path/to/native-tools checkout dd44bbdc75437332ed22e3ba95126733419ef25a
bash /path/to/native-tools/tools/setup-native-dependencies.sh
make -C /path/to/native-tools libc
export LLVM_CONFIG=/usr/bin/llvm-config-18
export NX_PYTHON=/path/to/recompiler/python
bash ps5/build_runtime_app.sh /path/to/native-tools /path/to/private/app-build
```

If `PS5_PAYLOAD_SDK` is already set, it must point to the SDK inside that pinned
toolchain. The script verifies the revision and clean source inputs, rebuilds
the host tools as needed, checks the companion runtime's recorded hash, builds
the app, converts/signs its native executable, and verifies mapped segments use
16 KiB alignment. `native-app-manifest.json` records every packaged file's hash.
The package uses the toolchain's placeholder icon, backgrounds and selection
audio; its license is included. No GitHub build is involved.

Copy the complete `package/PPSA98731` folder to `/data/homebrew/PPSA98731` and
verify its files, publishing `sce_sys/param.json` last so scanning cannot register
an incomplete title. Do not overwrite an existing title of that ID without first
establishing it belongs to this test. ShadowMountPlus can then register it through
its [scan API](https://github.com/drakmor/ShadowMountPlus/blob/1.7/docs/api.md).

Launch **Pokopia Runtime Tests** from the PS5 home screen. This headless suite
normally displays a blank screen while running and exits when finished. It writes
`/data/pokopia-recomp/tests/app-platform-<time>-<pid>.log`, containing
`context=native-app title=PPSA98731`. The current baseline has 226 cases; enabling
the game-image check makes 227, and the optional rtld or SDK probe makes 228. Only the expected complete report with zero
failures, together with a clean application exit, establishes success. Registration,
FSELF inspection and payload-suite results do not establish application runtime
behavior. Native game boot and gameplay remain separate checks.

On 2026-11-03 the first launch failed before tests because stderr redirection
assumed an open descriptor. Opening both report streams explicitly fixed that.
The next launch exposed native shared-object creation and missing thread-local
destructors; those fixes are now verified in the application process.

Report `app-platform-1793770437-125.log` contains **102 cases, zero failures**,
including the full 256 MiB heap case. That executable's SHA-256 is
`ab07c8e59e877b773923d5937fef3335697bd847c1a609c3d5983919b92ea45c`.
The kernel then recorded `exit_value=0` followed by SIGSYS: the remaining error
occurred after the tests, in the CRT's ordinary process-exit path.

`ps5/app/lifecycle.cpp` now drains main-thread TLS, records the result, flushes
the report and requests closure through `sceSystemServiceLoadExec("exit", nullptr)`.
If that call returns, it records the result and waits for shell closure; it never
falls through to the invalid exit syscall. Test failures remain visible in the
report and recorded main status. Resources that require orderly application
shutdown must be closed before returning from main; shell closure does not run
the CRT's global finalizers.

The shutdown build passed on the console: report
`app-platform-1793770766-128.log` contains **102 cases, zero failures**, followed
by `main returned status=0`. ShadowMountPlus recorded the process closing without
a game-error event, and the user confirmed a clean return home. Executable SHA-256:
`72cd362dc21be414345ecafb783be3bc8483b835639e944969c05f2e62f0c552`.

## Loader integration checks

`ps5_loader_tests` runs the 15 upstream loader cases plus a native protection
failure case against the PS5 memory backend. The fixtures are original AArch64
ELFs tracked under `runtime/tests/data`, with source and rebuild commands under
`runtime/tests/fixture`. They test mapping, symbols, RELA/RELR relocations,
import resolution, dispatch, initialization/finalization, TLS templates and
malformed headers. PS5 permission checks attempt real checked guest writes and
verify Horizon permission metadata, preserving the distinction between guest
ARM text and host-executable x86 code.

The protection-failure case verifies that a failed operation leaves the module
unlinked, that unloading releases its reservation, and that a fresh load works.
A partially linked image must be unloaded before retry; it must not execute.

On 2026-11-03, the payload suite passed **16 cases, zero failures** and exited.
Report: `loader-runtime-1793771058-133.log`; ELF SHA-256:
`47db2bae63420b91a99fe4ae58dcb5700aa05ef3a859c5d2b8bb0d44d2e08d45`.
For a payload run, copy the three `fixture_*.elf` files to
`/data/pokopia-recomp/tests/loader-data`; the native app packages them under
`tests/data` and reads them from `/app0`. Neither test uses game files.

The combined native app passed **118 cases, zero failures** on 2026-11-04.
Report `app-platform-1793773230-134.log` records main status zero; ShadowMountPlus
recorded process 134 closing without a game-error event, and the user confirmed
a clean return home. Executable SHA-256:
`812ca41be16544a750f31211c3995ffe3a411f5c7bb18259604f5aa598edbba3`.

## Decrypted game-image check

The optional next check maps the real `main`, `sdk`, `subsdk0` and `rtld` NSOs at
their compiled addresses. It does not register or execute the lifted game code,
call game constructors, apply host relocations, access RomFS or touch saves.
The normal 226-case suite still needs no game files. To build the additional
case, supply the dump as the third argument:

```sh
bash ps5/build_runtime_app.sh /path/to/native-tools /path/to/private/app-build /path/to/decrypted/Pokopia
```

`prepare_game_image_test.py` first validates the supported file hashes, then
independently decompresses all 12 NSO segments with Python LZ4 and verifies their
NSO SHA-256 hashes. It writes expectations and `game-image-inputs.json` under
`private-game-images` in the build directory. Keep these generated files private.
The executable checks the input header, length and CRC32; mapped segment CRC32s
are compared with the independent Python/zlib results, and all BSS bytes must be
zero. This tests the runtime's decompressor and mapping, not a second use of the
same implementation as an oracle.

Stage only the four named files from that manifest under
`/data/pokopia-recomp/game/exefs`, verify each copy against its recorded SHA-256,
and retain a `source-manifest.json` in the parent directory. Preserve any existing
unidentified input folder. Original PC files remain unchanged. The app package
does not bundle these files; its manifest records `requires_game_images: true`
and `expected_cases: 227`. Omitting the third build argument restores the normal
suite. The standalone diagnostic target is `ps5_game_image_tests`.

On 2026-11-04 the standalone diagnostic passed and exited: all four modules were
mapped together, their 82,247,680-byte combined image span was checked, segment
permissions were applied, and unloading released the mappings. It also reserved
the stack, kernel and 12 GiB heap address ranges; that heap remained physically
uncommitted. Report: `game-images-1793773633-137.log`; payload SHA-256:
`f9e96ab8fab1828e7696199b7f43d9bf36295d24f421f524160eced563a30221`.

The combined 119-case native build failed on 2026-11-04. Report
`app-platform-1793773881-138.log` ends with libc internal-memory exhaustion and
`std::bad_alloc`. The symbolicated abort stack reaches `SharedHeap::SharedHeap`
from `heap_create`, before the first game file is loaded. Its 12 GiB virtual heap
needs several 3 MiB metadata vectors; no 12 GiB physical allocation was attempted.
That failed executable's SHA-256 is
`77b1829775910efbd86412f78dbae9d8add60fe1d28b0c8a70d4321193ed48c2`.
Game execution, SVC/service integration, graphics and gameplay remain ahead.

## Native application allocations

`ps5/app/heap.cpp` routes the application and its static libraries' malloc family
through platform mspaces backed by owned PS5 direct memory. The pinned native
converter already sets an expandable libc heap in its process parameters; that
setting did not prevent the internal allocator failure above. The separate
mspace approach also appears in the working Eglantine PS5 native port.

The heap grows in spaces of at least 32 MiB, with larger spaces for larger requests.
Each space is assembled from 2 MiB physical pieces with 16 KiB alignment inside
one owned virtual reservation. CPU allocations stay in the range starting at
`0x10_0000_0000`, below `0x40_0000_0000`, away from the intended graphics window and
fixed guest images. The actual GPU integration must still validate the combined
layout. No game heap, shader, graphics or performance setting is reduced.

Space records are fixed-size and published atomically; allocator bootstrap never
uses malloc, C++ new or thread-local storage. Existing mspaces service concurrent
calls, with a separate lock only for growth. Freed blocks are reused. Spaces stay
mapped until application process teardown so late destructors cannot use freed
backing. There are at most 256 spaces; growth also obeys the platform's available
memory. Empty-space reclamation and allocator throughput under game workloads
remain to be measured. Physical allocation failures roll back the new reservation
and its backing; an unmap failure retains the backing until process teardown.

Foreign pointers from system libraries go back to their original allocator.
`calloc` checks multiplication overflow; a failed `realloc` preserves its input;
`posix_memalign` preserves the output and errno on failure. Seven native tests
cover simultaneous 96 MiB of C++ buffers, resize/zero/overflow semantics,
alignment, foreign-pointer ownership, concurrent allocation and cross-thread
freeing, large-buffer reuse, and rollback after an injected physical-allocation
failure. The test source disables allocation-elision builtins so the compiler
cannot replace the overflow and null checks without calling the allocator.

The test runner now reports uncaught C++ exceptions as failures and continues to
its normal shutdown, instead of aborting without a final report. Failed tests
still produce a nonzero main status.

The standalone `ps5_app_heap_tests` diagnostic exposed a separate payload budget
limit: it reported direct size zero and `0x80020023` while growing beyond about
96 MiB of backing. Smaller allocation, alignment, foreign ownership and threading
checks passed; the full diagnostic did not pass. Changing the physical piece size
or alignment did not remove that limit. Payloads run in the system budget rather
than a native title budget, as documented by the
[PS5LM console probes](https://github.com/abhi245y/PS5LM-Plus/blob/main/docs/CONSOLE.md).
These results do not establish the new heap's behavior in a native application.
The native application passed **126 cases, zero failures** on 2026-11-04.
Report `app-platform-1793775032-153.log` includes all seven heap checks and all four
game images, then main status zero. ShadowMountPlus recorded a clean exit, and
the user confirmed returning home. Executable SHA-256:
`ac69369e251a9bd72f89d14cd4780fe0c9b668ea31e8c42f9194017340641f9b`.

The native process reported a 12 GiB direct-memory budget and an initial largest
free block of 12,849,250,304 bytes. Its application heap grew to 337,641,472 bytes
across seven spaces; about 8 MiB remained live after the suite. The final recorded
allocation error is the deliberately injected rollback-test failure, not a failed
test. This resolves the observed game-image allocation failure; no guest game
code or gameplay ran. The payload's zero direct-memory budget is not the native
application's budget.


## Host worker lifecycle

`nxrt/host_thread.h` and `src/os/host_thread_posix.cpp` provide the host-thread
owner needed by the guest kernel port. Creation starts a POSIX worker behind a
gate; entry execution begins only after `start()`, allowing register and handle
initialization before dispatch. A join on an unstarted worker cancels that gate.
The owner joins during destruction, including when ownership moves to another
thread. Self-joins are rejected; repeated/concurrent host joins are serialized.
Uncaught entry exceptions are retained for the owner rather than escaping through
pthread startup. Explicit thread exit runs the cleanup handler and the native
CRT's C++ TLS cleanup.

`wait_for()` observes entry completion, not completion of TLS destruction.
`join()` establishes full termination. This distinction must be preserved by the
guest join implementation. A bounded `join_for()` also waits for TLS cleanup;
a timeout leaves the worker owned and permits a later retry. Stack size is an explicit creation parameter;
zero uses the platform default. No default game stack size, affinity mapping or
priority policy is selected by these tests.

Ten cases exercise startup gating, cancelled gates, join ownership, timeout
behavior, exception transfer, explicit exit/TLS cleanup, self-join rejection,
invalid inputs and stack sizing, repeated worker creation, and four workers
executing the original lifted LDAR/STLR fixture with independent register state.
The host suite at that milestone passed 67 cases plus the six existing TLS-runtime cases.
The console payload check also passed all 76 platform cases and exited.
Report: `platform-1793775610-157.log`; ELF SHA-256:
`8f3b992c027f92e0ca4ccef53b3003379ba3df95dfa0bedd640dbce08a379432`.

The combined native application passed **136 cases, zero failures**, and returned
home on 2026-11-04. Report: `app-platform-1793775707-158.log`. Executable SHA-256:
`d8cf633da4d013309dec34fe14c14acdc66047df1450bfdb865daf4a0ad30a0f`.
This is the verified native baseline before the guest-kernel additions.

This establishes the host worker foundation. It does not start a Pokopia thread
or execute game code.

## Guest threads and synchronization

`ps5/kernel.cmake` builds `libnxrt_ps5_kernel.a` with the shared handle table and
Horizon wait queues, plus POSIX guest-thread and clock backends. Each guest
thread owns its registers, kernel-region TLS and a gated host worker. Handles
can close while a worker runs without destroying its context. Completed workers
are joined outside the thread-list lock; reset refuses to discard live threads.
Explicit guest exit and C++ exceptions both mark the thread waitable. A host join
includes TLS destruction and retains ownership when its deadline expires.

The wait queues use the existing Horizon mutex-word, process-wide-key and address
arbitration protocols, with priority/FIFO handoff and remembered cancellation.
Timeout conversion now uses the registered platform frequency instead of assuming
19.2 MHz for every game. Repeated-wake tests cover both 19.2 and 31.25 MHz clocks.
Finite deadlines saturate safely, and 32-bit address-signal arithmetic wraps
without signed-overflow undefined behavior.

The 22-case kernel diagnostic passed on PS5 and its process exited:
`kernel-runtime-1793776543-162.log`; ELF SHA-256
`92e3ef571bcc72fd53f8eb5d951cea809140d08ef06c7ffa20a857ba94af7832`.
It covers template TLS, thread exit and ownership, handle limits, adopted-main
stacks, mutex priority ordering, 1,000 producer/consumer handoffs, condition
transfer timeouts, address signaling, waitable lifetime, cancellation and clock
regressions. Four guest workers also dispatch the original lifted LDAR/STLR
fixture. The first run found an incomplete TLS test setup: the fixture must be
linked before reading the process template. The corrected test passed.

The thread component is now connected to the SVC dispatcher as described below.
Host affinity/priority mapping, exception handling, services and gameplay remain. Guest priorities
currently determine kernel queue order; POSIX workers inherit the application's
host scheduling policy. The 64 MiB host-stack default is retained from upstream,
while these tests explicitly request 2 MiB. Game stack usage must be measured
before changing that default. No PS5 scheduling or 60 FPS claim follows from the
thread tests.

The updated host suite passes 68 platform cases and six TLS-runtime cases. The
PS5 platform diagnostic passes 77 cases, including a timed join that expires
while a TLS destructor is deliberately blocked, then succeeds after release.
The implementation uses the absolute-deadline join interface documented by
[FreeBSD's pthread implementation](https://github.com/freebsd/freebsd-src/blob/main/lib/libthr/thread/thr_join.c),
with a monotonic budget for retries. Both diagnostic processes exited.

The combined native application passed **159 cases, zero failures** in two launches
on 2026-11-04. Reports `app-platform-1793776905-165.log` and
`app-platform-1793776937-167.log` both record main status zero; both processes
exited, and the user confirmed returning home. Executable SHA-256:
`4c4268f316fb4f39e5cf38f5cfd7d530e93ba2c1ce9b09295125cc64cf3f5693`.

## System-call ABI integration

The first `ps5/svc.cmake` milestone connected the shared dispatcher to 23 handlers for thread creation,
startup and exit, priority/core-mask metadata, sleep/ticks, events, handles,
synchronization, address arbitration and debug strings. Unported calls return
the dispatcher's explicit not-implemented result and retain their call-site
records. Four memory handlers, five process-information handlers and three
synchronous IPC handlers have since been added, bringing the total to 35.
Process exit, processor-number mapping, exception return, thread activity and
preemption-state handling also remain unported.

Handler registration uses atomic publication. Event state is atomic, and resetting
a signal consumes it once even with concurrent callers. Writable/readable event
handle roles follow the [Atmosphere event implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_event.cpp)
and [synchronization implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_synchronization.cpp).
PS5 handlers check the full guest buffer's permissions before accessing handle
arrays, debug strings and synchronization words, including mixed 4 KiB guest
permissions within one 16 KiB host page. This preflight does not yet provide
fault recovery for a concurrent guest unmap. The newer 64-bit address-wait mode
is unsupported; the current backend implements the three 32-bit modes.

The separate `ps5_svc_tests` diagnostic passed **19 cases, zero failures** on PS5,
and its process exited. Report: `svc-runtime-1793777448-173.log`; ELF SHA-256:
`19e8178934a64c631cbfb8f432880a3cb5bf522f5b54db65843acd0dd85b837c`.
Cases cover register conventions, missing calls, concurrent registration,
event roles and reset races, handle-allocation rollback, guest-buffer permissions,
thread lifecycle/cancellation, clocks, mutex/address protocols and diagnostics.
Four original ARM fixture functions, containing 66 translated instructions,
execute through the real SVC ABI: reading the clock, creating/starting/joining
a child, exiting that child with independent TLS, and an event signal/reset cycle.
These are test programs, not Pokopia game code.

The combined native application passed **178 cases, zero failures** on 2026-11-04.
Report `app-platform-1793778483-174.log` records main status zero, the process
exited, and the user confirmed returning home. Executable SHA-256:
`eb9dfdf40185aae7254b9c849f371e863c678bc157b18f2afd294d384ad52383`.
The host checks pass 68 platform cases and six TLS cases, and the production SVC
archive builds with the PS5 SDK. All four ABI 4 game archives still pass the
module verifier. These results do not establish game startup or performance.

## Boot memory system calls

The PS5 SVC component now registers `SetHeapSize` (0x01),
`SetMemoryPermission` (0x02), `SetMemoryAttribute` (0x03) and `QueryMemory` (0x06).
They use the native sparse-memory backend through the same ABI as lifted ARM
code. The memory-budget function is shared with the Windows information group;
its 10.5 GiB Switch 2 value is the upstream guest resource limit, not a measurement
of free PS5 memory. Large real-game heap allocation remains untested.

Heap resizing preserves the retained prefix and zeroes newly exposed bytes,
including after shrink/regrow. Rejected sizes and failed native operations keep
the previous size and metadata. Shrinking a protected or guest-aliased tail is
rejected. The shared backing owner still retains physical blocks until teardown;
physical reclamation and GPU/private-view lifetime need further integration.

Permission changes support restoring committed no-access pages and retain the
4 KiB guest policy within 16 KiB host pages. Permission locks merge overlapping
or adjoining intervals, are idempotent, and prevent later reprotection. Queries
split their reported regions at attribute boundaries and check the complete
40-byte output buffer before writing. Validation follows the
[Atmosphere memory SVCs](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_memory.cpp),
[page-table implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp)
and [query-memory implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_query_memory.cpp).
Concurrent teardown or direct memory-API mutations still require runtime lifecycle
coordination; these handlers do not provide guest fault recovery.

Uncached attributes return not-implemented until PS5 cache/GPU integration.
Map/unmap, shared/transfer/physical-memory calls, cache maintenance and GPU
attributes remain unregistered on PS5. The existing Windows mapping handlers
are retained; the Windows runtime suite was not run for this milestone.

The expanded system-call diagnostic passed **37 cases, zero failures** on PS5,
then exited: `svc-runtime-1793779279-182.log`. ELF SHA-256:
`22fa7a96efbd47077bbca86f9c93e2d570e37822648e571d7e3debe8c604140d`.
The 18 added cases exercise heap contents, budgets, invalid ranges, no-access
restoration, query output boundaries, RELRO locks, concurrency, native-failure
rollback and reset. The original ARM fixture now contains five functions and
97 translated instructions; its new program sizes, writes, protects, queries
and releases a guest heap. The first run had one incorrect test expectation:
a query after protecting one page must report that page's region, not the whole
heap. Correcting that expectation produced the passing run above.

The **196-case native application passed with zero failures** on 2026-11-04.
Report `app-platform-1793779537-188.log` records main status zero; the process
exited and the user confirmed returning home. Executable SHA-256:
`866a84d3f1bf438d01555afeaf2d13e4f40a60926846360584a8b7fccf6e295e`.
Host checks pass 68 platform cases plus six TLS cases. The production SVC archive
builds and all four ABI 4 game archives pass verification. No Pokopia guest code,
rendering or gameplay has executed in this milestone.

## Process information and the real loader entry

The process-information milestone brought the PS5 runtime to 32 SVC handlers, adding `GetInfo` (0x29),
`GetSystemInfo` (0x6F), `GetProcessId` (0x24), `GetThreadId` (0x25) and
`GetProcessInfo` (0x7C). These provide the guest address layout, title metadata,
process/thread identity, platform limits and a shared exception-context page.
Title defaults are separated from the Windows service host so the same game
metadata and configuration precedence can be used on PS5. The context-page
cache must be reset between process-memory lifetimes.

Handle/subtype validation follows the
[Atmosphere information SVC implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_info.cpp).
Memory usage is still the existing guest model (heap plus mapped images plus
64 MiB overhead), not measured native allocation usage. Entropy retains the
upstream deterministic seed. Resource-limit handles, idle/thread CPU accounting,
other process pools and system-resource usage return not-implemented on PS5.
Resource-limit SVCs 0x30/0x31/0x37 remain unregistered. Windows accounting remains
on its existing backend; the Windows runtime suite was not run.

The expanded SVC payload passed **47 cases, zero failures**, and exited:
`svc-runtime-1793780091-208.log`; ELF SHA-256
`6779c6722e80270201cf074741cfd3b9f46970268921fe3c2905ccf868a4bb9a`.
Host checks pass 68 platform cases and six TLS cases. The production SVC archive
builds, and all four ABI 4 game archives pass the module verifier.

An optional test now executes the supplied game's recompiled `rtld` entry. It
validates and maps all four decrypted images, registers the 37-function rtld
module, adopts a main guest thread and enters the real loader. Diagnostic guards
allow only the loader's expected memory/information/debug system calls and stop
at the `nn::init::Start` import. Unexpected imports, unsupported calls, excessive
calls or a 20-second deadline fail the probe. The SDK startup boundary is an
explicit diagnostic stop, not an implementation of SDK startup.

The two-case payload passed on PS5 and exited:
`game-images-1793780373-215.log`; ELF SHA-256
`45c1c2abe8c109a1396018d2cbb6cc198181df48483d1a2a37b12b4133e02ab7`.
The real loader made 23 successful `QueryMemory` calls and reached SDK startup
with its main-thread handle and three rtld callbacks. The callbacks were not
executed; relocation completion, SDK startup, graphics, audio, saves and gameplay
remain unverified. The first probe stopped before dispatch because its fixture
expected one undefined import; this image has two. The corrected fixture guards
the exception-handler import separately and accepts only the startup import as
success.

Build this private native diagnostic locally by supplying a decrypted input and
an explicit fourth argument:

```sh
bash ps5/build_runtime_app.sh /path/to/native-tools /path/to/private/app-build /path/to/decrypted/input rtld
```

Without `rtld`, the optional image test only maps and checks the inputs. With
`rtld`, the package includes game-derived recompiled code and must stay outside
Git. `native-app-manifest.json` records `executes_game_rtld` and the expected case
count. The **207-case native application passed with zero failures** on
2026-11-04. Report `app-platform-1793781216-238.log` records the real loader
handoff and main status zero. A repeat launch in
`app-platform-1793781916-246.log` also passed all 207 cases. Both processes exited
and the user returned home.
Executable SHA-256:
`ee29203fe7bfa8d3e96de136423a518a2df1d86e75dde955c1b82c1726145c90`.
A later launch of the old 196-case app, PID 210, left an incomplete report ending
at `stress_random_ops`; its cause is not established, so that launch is not
counted as a pass. The later 207-case launch completed this stress test and the
remaining suite; it does not establish the cause of the earlier interruption.

## Synchronous IPC and the SDK initialization checkpoint

The PS5 runtime now registers `ConnectToNamedPort` (0x1F), `SendSyncRequest`
(0x21) and `SendSyncRequestWithUserBuffer` (0x22). These connect lifted code to
the shared HIPC/CMIF/TIPC engine, service manager, sessions and domain objects.
Named ports require a readable, terminated name of at most 11 characters.
Unknown ports return an error. The IPC message is copied before parsing so
descriptor validation and dispatch use the same message. Buffer descriptors are
checked against the full 4 KiB guest permission map, including mixed permissions
within a 16 KiB host page. Invalid handles and malformed messages are rejected.
The range/alignment checks follow the
[Atmosphere port implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_port.cpp)
and [IPC implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_ipc.cpp).

The portable service registry and existing log-manager implementation are now
linked on PS5. `lm` opens real logger objects, assembles log packet fragments and
stores the destination setting. The Windows fatal-service implementation remains
on Windows; `fatal:u` and `fatal:p` are not registered on PS5. Unknown service
commands retain explicit errors, rather than succeeding with empty replies.

The SVC payload passed **60 cases, zero failures**, then exited on 2026-11-04:
`svc-runtime-1793782279-252.log`; ELF SHA-256
`294bf2ce8db4f2bc3bc633fe1656a89fb24750be3caed0cc4542d8cfbd88b7fc`.
The thirteen new cases cover connection ownership, string and descriptor bounds,
CMIF/TIPC dispatch, TLS permissions, user-buffer validation, session cloning,
domain requests, error replies, logger lifetime and fragmented log packets.
The production PS5 SVC archive and host SVC archive compile. Host platform/TLS
checks passed earlier in this change; the Windows runtime suite was not run.

The first SDK probe linked the verified local SDK archive and continued from the real
rtld into `nn::init::Start`. It executed the first rtld callback, registered with
`sm:`, opened `lm` and obtained a logger. It deliberately stopped before dispatching
the SDK's request for `appletOE`, which was unported at that milestone. Unexpected imports,
unsupported system calls, stub services, unhandled commands or the watchdog
deadline fail the probe. This boundary is a diagnostic stop, not an applet-manager
implementation or proof that SDK initialization has completed.

The two-case payload passed and exited:
`game-images-1793782424-255.log`; ELF SHA-256
`f2ed89478854b5e0701ad8c65488f095bc64996bb8708d6e4f9c1ff2a909975d`.
It records 57 completed system calls and the intercepted 58th call, one returned
rtld callback, and no stub-service or unhandled-command use. This is execution of
the recompiled SDK, but does not establish complete relocations, applet services,
title startup, graphics, audio, saves or gameplay.

Build the private native version after building all four module archives:

```sh
bash ps5/build_runtime_app.sh /path/to/native-tools /path/to/private/app-build /path/to/decrypted/input sdk /path/to/private/build/modules
```

The script requires a module build belonging to this checkout, rebuilds `pkp_sdk`
and verifies all four archives before linking. The package manifest records
`executes_game_sdk: true` and the current expected case count. Keep this package
and its generated code private. The first SDK native executable was 130,071,891 bytes; SHA-256:
`c11c0c2e7cd775fc2a94aec0bd1662874a55b547af01a46010afefd5568134f1`.
The **220-case native application passed with zero failures** on 2026-11-04.
Report `app-platform-1793782991-268.log` records the SDK boundary and main status
zero. The process exited and the user confirmed returning home. Its package is
retained privately for rollback.

Remaining IPC limitations include user buffers larger than 4 KiB, asynchronous
requests, guest-hosted servers, and mapping
lifetime coordination during concurrent unmaps. The buffer snapshot does not
pin descriptor backing or provide fault recovery.

## Process handles and the application proxy

The POSIX kernel now owns one guest process object per runtime lifetime. The
current-process pseudo handle resolves to it, and ordinary copies work with
`GetProcessId`, process information and process-scoped `GetInfo` queries. Its PID
is fixed when first used and reset with the kernel; old objects cannot become
the next process just because a handle number is reused. This is a single-process
model, not guest process creation, termination or scheduling.

The PS5 IPC entry point now stamps the caller's PID into messages that request
it and gives services temporary receiver-side copies of incoming handles.
Services retain shared object ownership when needed; the temporary handles are
closed after dispatch, including error paths. Failed allocation rolls back any
copies already made, and pseudo handles cannot be moved. The special header is
edited in the validated snapshot without rebuilding its CMIF padding. This follows
the [Atmosphere handle-table model](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/kern_k_handle_table.hpp)
and the process-copy request used by [libnx applet initialization](https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c).
Moved ordinary handles and concurrent handle-table mutation retain the existing
runtime limitations; this is not a full multi-process IPC kernel.

`am_ps5.cpp` implements the initial `appletOE` application proxy separately from
the Windows backend. Opening it validates the copied process object and PID.
Its interfaces own the message queue, readable events, focus/mode notification
state, foreground ownership and application state. Reset starts a fresh state;
already-open sessions keep their old state until released. The diagnostic starts
in focus and docked. PS5 shell focus, suspend and exit callbacks still need wiring.

The graphics-scale query returns the fixed full-quality guest value used by the
existing runtime. It does not establish rendering quality or performance on PS5.
The performance interface reports CPU overclocking disabled and explicitly
rejects attempts to enable it; no PS5 clock changes are made. Command names for
these Switch 2 interfaces follow the
[Applet Manager interface research](https://switchbrew.org/wiki/AM_services#IPerformanceFunctions).
At this checkpoint, library applets, display/audio controllers, saves and system-app
proxies were unported. Unknown commands return errors; this backend supplies no
generic success objects for missing interfaces.

The expanded SVC diagnostic passed **68 cases, zero failures**, and exited:
`svc-runtime-1793783666-279.log`; ELF SHA-256
`1b4b3d7b5dfd6f3d03ea6558290736931034b26138f0bcb9b00469d14ba9195b`.
New tests exercise process identity/copy lifetime, PID stamping, handle exhaustion
rollback, rejected pseudo moves, application-proxy validation, shared events,
message ordering, notification flags, reset isolation and fixed platform queries.
The host platform and TLS suites pass. The Windows runtime suite was not run.

The SDK probe at this checkpoint stops before `IApplicationProxy::GetLibraryAppletCreator`.
Before that stop it must open the application proxy and obtain three subsidiary
interfaces, including graphics-scale and performance, with no unknown commands.
This remains a bounded initialization check, not complete SDK startup or gameplay.

The two-case payload passed and exited:
`game-images-1793783824-280.log`; ELF SHA-256
`d9512c224542e14004699cb61e28eceb8ac5b0d113c800b5e0374c187d64f819`.
It records 64 completed system calls and intercepts the 65th before the missing
interface is created. The first rtld callback returns; later callbacks and title
startup have not run. The initial exploratory runs stopped on unported proxy
commands 60, 40 and 11; those reports are not counted as passing runs.

The **228-case native application passed with zero failures** on 2026-11-04.
Report `app-platform-1793784171-292.log` records the library-applet boundary,
main status zero and a normal process exit. The user confirmed returning home.
A repeat launch in `app-platform-1793784815-304.log` also passed all 228 cases
and exited normally before the next installation.
It was built locally with executable SHA-256
`5cf3dff54105762c1b98235a7aad1d28a9d2e1c725d0eaa635c129e612351004`
(130,245,843 bytes). Its installation and all package files were readback-verified;
the passing package is retained privately for rollback. The
production PS5 SVC archive compiles, and all four ABI 4 game archives pass the
module verifier. The game-derived artifacts remain private.

## Applet storage and the display-controller checkpoint

The PS5 library-applet creator now provides byte-backed `IStorage` and
`IStorageAccessor` objects. Accessors share ownership, so closing the creator or
storage session does not invalidate an accessor. New storage is zero-filled;
reads and writes check the complete range and guest page permissions before
copying. A mutex serializes access to each buffer. Empty buffers work without
pointer arithmetic on a null allocation. Negative sizes and allocations over
256 MiB are rejected, and allocation failures return an applet error.
Transfer-memory storage and UI applet creation explicitly return not-implemented;
they do not create empty success objects. Buffer backing is not pinned against
concurrent guest unmapping, which remains an IPC limitation.

`IAudioController` stores and returns the application's expected main/library
volumes. Values are clamped to 0–1; non-finite input fails without a partial
update. Reset isolates old sessions from the next application state. This is
volume-request state only: host audio output, mixing and fades remain unported.
Command layouts follow the existing runtime and
[libnx's applet/storage implementation](https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c).

The expanded SVC diagnostic passed **73 cases, zero failures**, and exited:
`svc-runtime-1793784579-297.log`; ELF SHA-256
`e0771fcf4350f4f31ffc9ce68ee3fc0daf32b779df5d74b58575e119985ce0d0`.
The five new cases cover shared buffer lifetime, A/X input and B/C output
descriptors, zero initialization, atomic range failures, empty buffers, size
limits, guest permissions, unavailable applets and volume-state isolation.

At this checkpoint, the SDK probe traces each requested service and command. It passes eight
application-proxy calls and retrieves the window resource ID, then stops before
`GetDisplayController`. The SDK obtains the storage-capable creator and audio
controller; it does not use their storage/volume commands during this bounded
probe. Those operations are exercised by the SVC cases above. Display capture,
graphics, full SDK/title initialization, audio output, saves and gameplay remain
unverified. The first rtld callback returns; the other two have not run.

The final two-case SDK payload passed and exited:
`game-images-1793784811-303.log`; ELF SHA-256
`86dc1b53a54ff61e26e54966f5b5d3d93e9db8f81964fcf423eb93c295a69e9d`.
It records 70 completed system calls and intercepts the 71st before creating the
missing interface. No unknown service or unhandled command was used. The
production SVC archive builds, and all four ABI 4 game archives pass verification.
The Windows runtime suite was not run.

The **233-case native application passed with zero failures** on 2026-11-04.
Report `app-platform-1793784998-309.log` records the display-controller boundary
and main status zero. The process exited, and the user confirmed returning home.
Every package file was readback-verified before that launch. Its
executable is 130,347,955 bytes; SHA-256:
`eb9610dee11f44e2326c84e357ddc083b001f4a766646327a4fcc651ebff0c21`.
The passing package is retained privately for rollback. This diagnostic
still stops during SDK initialization and does not launch gameplay.

## Applet lifecycle and SDK stack mapping

The application proxy can now return typed display/debug endpoints during SDK
setup. Their operations explicitly return errors: no capture images, GPU buffers,
debug actions or host-shell operations are supplied. The SDK probe fails if it
actually uses either endpoint. Obtaining these handles is protocol progress, not
a renderer implementation.

The PS5 applet backend stores focus/suspend policy and the next-program exit
notification request. `SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled`
takes no argument; it enables the stored flag. Suspended-time accounting and the
GPU-error event have host-facing entry points in `nxrt/am_ps5.h`. Tick updates
check overflow before changing state, signal shared readable events and queue a
resume notification only when requested. GPU-error handles share a latched event.
Reset keeps old sessions and event readers isolated from the next application.
Fatal sections now nest and reject unmatched leaves.

The shell, scheduler and renderer still need to call these entry points. The
accounting API does not suspend workers, and the error event does not detect
hardware faults by itself. Command layouts follow the
[Applet Manager interface research](https://switchbrew.org/wiki/AM_services#ISelfController).

The final SVC payload passed **79 cases, zero failures**, then exited:
`svc-runtime-1793785624-318.log`; ELF SHA-256
`403b5768199be393f027cc826463000b87c68eca8cf59e1ace8da10fab00ba49`.
The six new cases cover unavailable display/debug operations, lifecycle state,
suspended ticks and notifications, overflow/reset isolation, fatal-section
nesting and GPU-error event ownership.

The bounded SDK probe now checks CMIF result codes at the call site, including
domain replies. It reaches the SDK's 4 KiB stack-mapping request after obtaining
the suspend and GPU-error events. It validates the source page, destination region
and requested size, then stops before `MapMemory` executes. This is not a mapping
implementation, SDK worker launch or completed initialization. The early probes
exposed command 900's missing handler, an incorrect argument assumption, the GPU
event dependency and finally unported SVC 0x04; those runs are not passes.

The final two-case SDK payload passed and exited:
`game-images-1793785827-322.log`; ELF SHA-256
`1bff74ecc0b8d8447770bcc5e9b464b2a8f449c6aa08b2757bcf3b20b39ed1e0`.
It completed 77 system calls and intercepted the 78th: a 0x1000-byte mapping
from SDK address `0x7500e14000` to `0x7caa2f0000` in the stack region. No display
or debug operation was invoked. All preceding checked CMIF requests succeeded,
and no unhandled service command was used. The first rtld callback returned;
later callbacks and title startup have not executed. The next memory work must
preserve guest page ownership and permissions across PS5's larger host pages.

The production PS5 SVC archive builds, and all four ABI 4 game archives pass the
module verifier. The Windows runtime suite was not run.

The **239-case native application** is built locally and installed, with every
package file readback-verified. Both native launches passed all 239 cases with
zero failures and main status zero; their processes exited and the user confirmed
returning home. Reports: `app-platform-1793785879-327.log` and
`app-platform-1793786160-332.log`. Its
executable is 130,448,787 bytes; SHA-256:
`8aa3e8ba8fca49cc7e93738779074a6f237018f2957ee6856f44aea1c036c80f`.
The passing 233-case package is retained privately for rollback. The installed
diagnostic stops before SDK stack remapping and does not launch gameplay.

## Stack mapping and automated startup tracing

PS5 SVCs 0x04/0x05 now move exclusive ownership between writable heap/module
pages and stack addresses. The source becomes inaccessible and reports the
Locked attribute; unmap restores its contents and permissions. Whole, prefix,
suffix and middle unmaps preserve the remaining ranges. Permission changes,
heap shrink, replacement and module release cannot bypass a live mapping.

This backend copies the bytes through temporary private backing views rather
than creating a physical alias. That supports arbitrary 4 KiB source/destination
alignment inside PS5's 16 KiB pages. It requires quiescent source/destination
buffers during transfer and rejects heap pages with overlapping guest aliases
or private/GPU import views. New aliases/imports are refused while mapped. A
source range must fit one fixed reservation or the primary heap. It is not a
shared-memory implementation and has not been performance-tested with gameplay.
The validation rules follow
[Atmosphere's memory SVCs](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_memory.cpp)
and [page-table implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp).

Private views and updated metadata are prepared before permission changes.
Failed protection restores the previous mapping; a failed rollback aborts rather
than leaving guest execution with inconsistent ownership. On PS5, the SVC
payload passed **87 cases, zero failures** in
`svc-runtime-1793786692-339.log`; SHA-256:
`75bc471be137a0318dc03a71c70a83ce55348f15d59b7729e15e9e3732a6a03c`.
The eight added cases exercise data round trips, neighboring subpages, partial
unmap, invalid/occupied ranges, aliases/imports, lifetime locks, both permission
failure points in each direction, and a copy across physical backing blocks.

The bounded SDK test maps the real 4 KiB SDK stack, creates its worker with the
SDK-provided registers/TLS, and starts its recompiled entry. It checks the first
mutex request against the parent handle, mapped mutex word and worker tag, then
stops before waiting and joins the worker. Report
`game-images-1793787424-350.log` passed both cases and exited; ELF SHA-256:
`e0d0541045253cd609337fbea17fec2981d2683a8c53edc11f317d63b7166248`.
This checks worker entry, not completion of the SDK thread's initialization.

A separate `ps5_sdk_startup_trace` payload allows already-ported SVCs to execute
past that checkpoint, including parent/worker synchronization. Build it from the
same SDK-enabled test configuration:

```sh
cmake --build /path/to/private/build/app-tests --target ps5_sdk_startup_trace
```

It logs `/data/pokopia-recomp/tests/sdk-startup-<time>-<pid>.log`, limits execution
to 1,000 SVCs or 60 seconds, and exits its own process and workers on an unknown
service/call, checked CMIF error, external import or limit. It deliberately does
not report a passing test. Its IPC diagnostic wrappers serialize requests while
inspecting domain objects; timings are not performance measurements.

Trace `sdk-startup-1793787201-349.log` shows the real mutex handoff completing,
the worker entering `WaitSynchronization`, and the main thread processing focus
and foreground setup. It stops at call 95: `IApplicationFunctions` command 320,
result `0x000bcbd5`. The original Windows backend returns two zero bytes for 320
and one for 321 but labels their meanings unknown; these compatibility replies
were not enabled at this checkpoint. Their consumers were subsequently identified
as described below. ELF SHA-256:
`4b3fe97d6fbe5a16e6156ca5cc4ad383e16fb99444044a56378129c20e8ce90d`.
Earlier exploratory SDK runs stopped on overly strict stack-top validation and
the first worker mutex request; those runs are not passes.

The locally packaged **247-case native app** includes the stack/worker test.
It is prepared for a later combined native test; it has not replaced the proven
239-case app or been launched through ShadowMountPlus. Automated payload traces
will cover larger startup chunks before another manual launch is requested.

There is still no rendered game menu. The path to a first real frame includes
completing SDK/title startup, linking the remaining game code into the runnable
app, providing RomFS/file services, and integrating the NVN/Vulkan renderer with
PS5 RADV/VideoOut. The existing renderer and filesystem contain Windows host
backends; they are not linked into this PS5 diagnostic. Audio, controller and
save integration follow alongside that work. No date or 60 FPS result is implied
by these startup checks.

## Startup, read-only storage and native graphics diagnostic

The next startup batch identifies the previously unnamed application commands
from the supplied SDK, build ID `cc7da00d672399efe7958790d17fae396b1fde9f`.
`nn::oe::InitializeApplet` at `0x7500515ba0` consumes command 320's two
bytes at `0x7500515d20`, then calls the exported
`SetScreenShotAndContinuousRecordingInitiallyEnabled(bool,bool)` at
`0x7500518430` through PLT entry `0x75007adb10`. Command 321's single byte
is consumed at `0x7500515d4c`: it sets buffer permission bit 4 and is passed
to `SetRtcScreenSharingAudioEnabled(bool)` at `0x75001c67a0`.
These are observations from the locally generated SDK translation and original
dynamic symbols, not recovered Nintendo command names. No SDK bytes or
translated game source are committed.

The PS5 replies now report guest screenshot, recording and RTC sharing disabled.
`ISelfController` 2400 accepts disabling RTC audio sharing and rejects enabling
it. The add-on service exposes an empty inventory and real shared notification
events; mounting/purchasing content is unsupported. Parental-control sessions
are bound to the current guest process and use an unrestricted local guest
profile; they do not modify PS5 shell/account settings. Error-context endpoints
can be opened, but persistence explicitly fails rather than inventing a context
descriptor. The endpoint layout follows
[Switchbrew's glue services](https://switchbrew.org/wiki/Glue_services#ectx:aw).

The read-only file backend and `fsp-srv` now serve real host files through a
RomFS image, with signed-range, buffer-capacity and guest-permission validation.
The combined PS5 SVC suite passed **99 cases, zero failures** in
`svc-runtime-1793788869-371.log`; ELF SHA-256:
`82fe0d0b811851ed6fea8dbbfc1c9601ce6a2a89a9877b1a43dae3af08c594d9`.
The separate reader passed **14 cases, zero failures** in
`romfs-runtime-1793788861-368.log`. Both processes exited. These use test files;
the complete game's 7.25 GB asset tree has not been staged or read on PS5.
See [RomFS build instructions and validation boundaries](ROMFS_PORT.md).

The SDK tracer now accepts the SDK's domain-object close messages and avoids
printing the complete IPC summary after every request. This fixes diagnostic
overhead and a false stop during teardown; it is not a gameplay optimization.
The earlier 20-second trace at call 107 expired and the next trace stopped on
the diagnostic's domain-close guard at call 110. Neither is a passing startup
test. The native runtime title `PPSA98731` remains the previously proven
239-case installation; these additional runtime cases have not replaced it.

The final automated trace `sdk-startup-1793789090-385.log` completes parental
service creation/initialization and domain/session teardown. It stops at SVC
call 131 on the next unported dependency, `time:u`; the process then exits.
ELF SHA-256:
`e1960b68b330c75b50b83c53d162b3d6a4f942ade85e383b9e2cc6c0c23e8812`.
This is dependency discovery, not successful completion of SDK startup.

`PPSA98732`, **Pokopia Graphics Test**, is a separate native application for
colored-quadrant clears, presentation, pacing and GPU pixel readback. It includes
no guest mappings, game code or assets. Every package file was readback-verified
at `/data/homebrew/PPSA98732`; its executable is 19,047,859 bytes, SHA-256
`be8b0cdc9aeefb04dd82c0b7324149939aff3c1f1d413ea1aa56f080bcdf9462`.
ShadowMountPlus reports the separate title installed and available.
Native display behavior still requires a manual console launch. Its independent
process avoids combining the driver's current VA allocation window with guest
addresses; that overlap remains a required game-renderer integration task.
See [graphics build instructions and remaining renderer work](GRAPHICS_PORT.md).

## Repository ownership

`origin` is `dakotadawolfe/Pokopia-Recomp-PS5`; `upstream` is the original
`drakolordx7/pokopia-recomp`. Commit only source, documentation and build tools.
Use `work/`, `game/` or `build/` for generated and game-derived files, all ignored
by the inherited `.gitignore`. No GitHub build workflow is required.
