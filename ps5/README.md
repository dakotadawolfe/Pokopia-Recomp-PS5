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
`libnxrt_ps5_core.a` and `libnxrt_ps5_kernel.a`. These are link inputs, not a finished game executable.
The platform library contains synchronization, logging, configuration, the
gauge registry and the PS5 guest-memory API, including sparse fixed regions,
shared heaps and aliases.
The core library contains dispatch, the generated-code ABI,
module registration, hooks, NSO loading and linking. The kernel component adds
guest thread objects, handles, TLS, timing and synchronization queues. SVC
dispatch, services, guest exceptions, write tracking and rendering need integration
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
contains original ARM fixtures and runtime tests, with no Pokopia code or assets.
It uses the native application CRT, kernel provider, C++ libraries and companion
`libc.prx`, rather than the payload CRT. Its 118 cases comprise 66 platform cases,
six thread-lifetime/exception cases, 30 memory API cases and 16 loader cases.

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
`context=native-app title=PPSA98731`. The current baseline has 158 cases; enabling
the game-image check makes 159. Only the expected complete report with zero
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
The normal 158-case suite still needs no game files. To build the additional
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
and `expected_cases: 159`. Omitting the third build argument restores the normal
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

This component is not wired into the SVC dispatcher yet. Host affinity/priority
mapping, exception handling, services and gameplay remain. Guest priorities
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

The combined 159-case native application is installed and all package files are
readback-verified. Its native launch check is pending. Executable SHA-256:
`4c4268f316fb4f39e5cf38f5cfd7d530e93ba2c1ce9b09295125cc64cf3f5693`.

## Repository ownership

`origin` is `dakotadawolfe/Pokopia-Recomp-PS5`; `upstream` is the original
`drakolordx7/pokopia-recomp`. Commit only source, documentation and build tools.
Use `work/`, `game/` or `build/` for generated and game-derived files, all ignored
by the inherited `.gitignore`. No GitHub build workflow is required.
