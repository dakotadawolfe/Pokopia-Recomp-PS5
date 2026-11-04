# Full native startup diagnostic

`ps5_game_startup_trace` is an opt-in diagnostic that links the real local
`main`, `sdk`, `subsdk0` and `rtld` ABI-4 archives. It registers all 200,586
discovered functions before resolving imports, protects the original mapped
images, and starts the supplied loader. Host relocation is not applied first.
The existing image, bounded loader/SDK and SDK-only trace targets retain their
earlier scope.

Every provider is registered before import resolution. Defined game exports
resolve to their real guest addresses; only absent imports receive stop
handlers. In particular, `nninitStartup` resolves to `main` at `0x71000A44E0`.
Its diagnostic wrapper logs entry and forwards to the real function. The trace
also checks that no import is forced to a host override.

The trace stops on an unsupported dependency, nonzero CMIF response, guest
exception-frame boundary, unknown system-register operation, untranslated
instruction, guest trap or unbacked call. It also stops after 10,000 system calls
or a 60-second execution watchdog. A service error can be a legitimate game
error path, so the captured result must be interpreted before calling it an
unimplemented feature. The diagnostic ends the whole process with status 2 and
`GAME_TRACE_END`; reaching that marker is never reported as a passing test or
gameplay. The watchdog starts immediately before guest entry, after input
verification and module registration.

This payload does not register a game renderer or launch menu, stage assets, or
implement saves. The separate native folder picker chooses existing storage;
there is no asset copy step here.

## Build

Build and verify the four private archives first using `build_modules.sh` and
`check_modules.py`. Then, from the repository root in Linux/WSL:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export NX_PYTHON=/path/to/python-with-recomp-requirements
input=/path/to/existing/decrypted/game
modules=/path/to/verified-module-build
build=/path/to/startup-build

cmake -S ps5/tests -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DNX_PYTHON="$NX_PYTHON" \
  -DPKP_GAME_IMAGE_INPUT="$input" -DPKP_SDK_MODULE_BUILD="$modules" \
  -DPKP_TEST_RTLD=ON -DPKP_TEST_SDK=ON -DPKP_TEST_GAME_STARTUP=ON
cmake --build "$build" --target ps5_game_startup_trace --parallel 4
```

The target is excluded from the default build even when enabled. It reuses only
the four verified game archives; runtime and services compile from current
source. `prepare_startup_trace.py` verifies current archive hashes against
`module-verification.json` and matches the module build's recorded input to the
independently prepared image manifest. It writes a private header and
`private-startup/startup-inputs.json`, including source/archive hashes, ABI,
function counts, identity and verification-report hashes. The program prints
the stamp and module identities before dispatch. Generated inputs, ELF files and
game-derived code stay outside Git.

Console executable inputs currently remain at
`/data/pokopia-recomp/game/exefs/`, as in the preceding SDK diagnostic. This
diagnostic does not yet consume the folder picker's saved path. It verifies
file lengths, headers and complete-file CRCs against the locally hash-validated
input before mapping them. Parent-side staging/readback SHA-256 remains separate
evidence.

## Evidence as of 2026-11-04

- Local full target compiles and links. All four linked ABI constants equal 4;
  the module descriptors and diagnostic guard functions are present.
- Existing `ps5_rtld_tests` and `ps5_sdk_startup_trace` still compile and link.
- Negative provenance checks reject a mismatched ABI and an archive changed
  since verification without emitting an output stamp. Temporary fixtures were
  removed.
- ELF: `/root/pokopia-recomp-ps5-build/game-startup/ps5_game_startup_trace`,
  **811,139,640 bytes**, SHA-256
  `3ebae73c23e9641c2e1c4430a345a6288bd920118c5333a12272aef1beffaf33`.
- Console report `game-startup-1793792302-409.log` registers all four descriptors,
  resolves 945 main, eight SDK, 357 subsdk0 and two loader imports to real code,
  and reaches `GAME_ENTER nninitStartup` in the recompiled main module. Two
  missing main imports retain stop guards and were not called in this trace.
- The game queries available/used application memory, then calls `SetHeapSize`
  with `0x297000000` bytes (10.36 GiB). The direct-memory allocator fails a
  2 MiB backing allocation with `0x80020023`; the SVC returns `0xd001`. The
  report ends there and PID 409 disappears without `GAME_TRACE_END`. This is
  not a successful or deliberately stopped startup run.
- This failure is from a payload process, whose direct-memory limit query
  returns zero. Earlier native-app diagnostics reported a 12 GiB direct-memory
  budget. Native application capacity must therefore be measured separately;
  this payload result does not prove the game exceeds the native app's budget.
  The current heap commits physical backing eagerly, which also needs review
  before treating a virtual heap request as the game's actual working set.
- The 811 MB diagnostic exceeded Payload Manager's HTTP upload limit. It was
  transferred to an owned temporary diagnostic path through FTP, read back in
  full to verify SHA-256, and launched through Payload Manager's local loader.
  No game assets were transferred. No menu, game renderer or gameplay ran.
