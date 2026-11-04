# Native Pokopia for PS5

The main goal is a native PS5 application using the existing static recompilation
and the user's decrypted Pokopia files. Development and builds run locally.
The initial upstream revision is `2c29515b23a6d78d967775fd7e9f24f50402cca7`.

## Product scope

- Load decrypted ExeFS and RomFS from `C:/Games/pokopia` during local preparation.
  An explicit input path can point to an existing dump elsewhere. Do not request
  keys or firmware; this application consumes extracted game files.
- Produce a native PS5 application and its executable, using the existing
  homebrew SDK, RADV and native presentation work where appropriate.
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
level match upstream. Their runtime memory layout remains to be implemented.

The component target produces `libpkp_main.a`, `libpkp_sdk.a`,
`libpkp_subsdk0.a`, `libpkp_rtld.a`, `libnxrt_ps5_platform.a` and
`libnxrt_ps5_core.a`. These are link inputs, not a finished game executable.
The platform library contains synchronization, logging, configuration, the
gauge registry and native memory primitives. The core library contains dispatch, the generated-code ABI,
module registration, hooks, NSO loading and linking. Memory, guest threads,
services, exceptions and rendering still need platform implementations before
the core can run the game.

The platform check runs 48 cases on the host: 17 upstream cases for mutexes,
condition variables, events and semaphores, 23 upstream cases for logging and
configuration, and eight memory cases. The PS5 build runs 49 cases, including
an additional native direct-memory ownership test. It writes a report
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
locally with the PS5 SDK. The 48 host platform cases passed. The PS5 suite was
uploaded, readback-verified and executed: **49 cases, zero failures**, followed
by a clean process exit. Test ELF SHA-256:
`9993227bdc44477b9eafff7bbc1ef1a2b630decdcf9186d064f8da2576e769ea`.
The saved report is `platform-1793764792-102.log` in the private `work/ps5/`
directory. This establishes platform behavior, not game boot or performance.

## Native memory foundation

`nxrt/host_vm.h` owns POSIX anonymous/shared mappings and rejects unaligned,
out-of-bounds or overflowing operations. Exact-address requests never replace
an occupied mapping. These primitives are useful for host tests and small
runtime allocations; large PS5 regions use `nxrt/ps5_vm.h` instead.

The PS5 implementation reserves virtual ranges with
`sceKernelReserveVirtualRange` and attaches direct-memory allocations only to
owned, previously unbacked subranges. Every view retains its backing until the
view is unmapped. Protection changes require complete backing and host-page
alignment. The guest heap must initialize new physical pages before exposing
them to game code.

Hardware findings that determine the implementation:

- Host protection pages are 16 KiB. A successful `mprotect` for 4 KiB also
  protected the neighboring guest page. The low-level wrappers reject that
  request instead of silently rounding it. Guest 4 KiB permission handling and
  write tracking remain to be ported; changing the guest page constant is not
  a solution.
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
wrappers rather than substitutes. All target results so far are payload-process
tests, not a launched homebrew game application.

Next: connect these primitives to the guest memory and loader contracts,
resolve 4 KiB guest permissions and write tracking, and partition the RADV
address arena before initializing graphics. The existing RADV arena spans
`0x4000000000` through `0x8000000000` and overlaps the lifted game layout.
Do not combine the allocators until their ranges have been separated and tested.

## Repository ownership

`origin` is `dakotadawolfe/Pokopia-Recomp-PS5`; `upstream` is the original
`drakolordx7/pokopia-recomp`. Commit only source, documentation and build tools.
Use `work/`, `game/` or `build/` for generated and game-derived files, all ignored
by the inherited `.gitignore`. No GitHub build workflow is required.
