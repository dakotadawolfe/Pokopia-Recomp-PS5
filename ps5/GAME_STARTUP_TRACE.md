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

## Native application diagnostic

`build_startup_app.sh` builds the same four-module trace using the native CRT,
direct-memory host allocator and shell exit path. It produces `startup.bin`,
`startup.elf` and `startup-app-manifest.json` in a separate local build directory:

```sh
NX_PYTHON=/path/to/python-with-recomp-requirements \
  bash ps5/build_startup_app.sh /path/to/pinned-native-tools \
  /path/to/native-startup-build /path/to/decrypted-game /path/to/verified-modules
```

This temporary headless diagnostic uses the existing **PPSA98732** identity so
it can read the folder choice saved by the graphics app. It does not open the
picker or renderer. It revalidates that selection, reads the four NSOs directly
from the chosen ExeFS and configures read-only RomFS from the chosen asset tree.
There is no fallback to the old staged ExeFS directory. Multiple asset layers
are rejected until overlay support exists. Files must match the build's full
length, header and complete-file CRC before mapping; the deployment workflow
also checks their full SHA-256 values against the private input stamp.

Before replacing the executable, retain the working graphics package. The
`replace_graphics_executable.py` tool checks the title marker, ShadowMountPlus
unmounted state, installed executable hash and native libc hash, then backs up
the executable, transfers to a unique staging name and verifies both staging
and final SHA-256 readbacks. It changes only `eboot.bin`. Restore the graphics
executable after collecting the native trace, using the same stopped-state
checks and the diagnostic's hash as `--previous-sha256`.

The one-shot `launch_graphics_app.c` helper can launch this diagnostic under
the same title. Its report is separate from `game-native-<time>-<pid>.log`;
inspect that native report and process disappearance before restoring the app.
Unsupported paths still produce `GAME_TRACE_END`, which is a diagnostic stop,
not a passing startup. Native stops use the shell exit request instead of the
payload's `_Exit` syscall. No game assets are transferred or modified.

The native link disables the generic GNU build-ID note. In this larger image,
LLVM places that note next to writable data; the pinned native converter counts
it as read-only content and rejects the layout for overlapping process parameter
blocks. SHA-256 and the private module provenance stamp identify this artifact.
Neither the pinned toolchain nor its native conversion checks are patched.

The first native startup launch (803,998,083 bytes, SHA-256
`df2788fe39443e40a6768ff6cde09345cd364ef251490f0daeb3f68879acd778`)
was installed and readback-verified. Launching it was followed by the console
restarting, confirmed by the user; all helper/service ports became unavailable.
No native startup report had been collected at that point. This does not yet
locate the failure at application loading, the heap request, or guest execution.

Subsequent native diagnostics default to stopping before any guest heap request
over 512 MiB, logging the requested size, native direct-memory limit and largest
free extent. The game still sees its original resource limit; the diagnostic
stops instead of shrinking its request or returning false success. The build
script explicitly sets this threshold, and the manifest records it. This
guard is not evidence that a smaller heap fixes the restart.

The guarded, checkpoint-flushing candidate (803,998,515 bytes, SHA-256
`e33af8cdc76fddf6021d5953b0f761fde5dbb563d5e855c855fd7c307c420b79`)
also lost the console connection immediately on launch. After recovery, the
durable helper report `native-launch-1793816688-95.log` ends at the launch
request. ShadowMountPlus's retained `debug.log.1` shows the title's nullfs and
data/USB mounts ready, followed by a shell-status polling error. No
`game-native-*.log` exists, including the checkpoint flushed at the beginning
of `main`. This narrows investigation to executable loading or earlier runtime
initialization; it does not establish insufficient game RAM or a specific
loader bug. The system error-history directory has no new entry for these runs.

Both startup candidates are failed console artifacts; do not repeat their
launches. The original picker/renderer executable was restored after each run
and fully readback-verified. The final restoration receipt is
`work/ps5/game-native-bounded-restore.json`, installed SHA-256
`e9ea79967824e29e6227b5267e5404519ad39407f61d724bb47bdc293462295d`.
Startup build and packaging checks pass locally, as do the existing loader and
SDK trace builds. Those checks do not establish that this large native image
can boot. No new gameplay or controller test is ready.

### Compact native startup candidate

The next candidate compiles the same generated code with Clang `-Oz`. The
default remains `-O1`; the optional third argument to `build_modules.sh` selects
the optimization for a separate module build. The native startup script reads
that build's setting and applies it to its generated fixtures as well:

```sh
export PS5_PAYLOAD_SDK=/path/to/pinned-native-tools/.deps/native/ps5-payload-sdk
export NX_PYTHON=/path/to/python-with-recomp-requirements
bash ps5/build_modules.sh /path/to/decrypted-game /path/to/modules-compact -Oz
bash ps5/build_startup_app.sh /path/to/pinned-native-tools \
  /path/to/native-compact /path/to/decrypted-game /path/to/modules-compact
```

On 2026-11-04, all four compact archives passed verification with ABI 4 and all
200,586 functions retained. All 288 generated C++ and header files are
byte-identical to the original module build. Archive sizes fell by 54–59%; the
native FSELF fell from 803,998,515 to **257,655,843 bytes**. Its SHA-256 is
`b24545c237fa9cd48390e82073765aae9b5d19a284dc4d20655d13dadf3d58b6`.
The native ELF is 263,152,668 bytes, SHA-256
`d65834f0d5525a94990576f1bc7f3a6fda1101de9d756be414488f0823ac63a7`.
The host platform suite compiled with the same generated-code option passed
68 cases with zero failures. Native packaging and structural checks also pass.

The candidate keeps the 512 MiB diagnostic heap stop and adds kernel-file
checkpoints immediately before and after `_init_env`, before `main` or C++
constructors. These append PID-tagged, flushed records to
`/data/pokopia-recomp/tests/native-bootstrap.log`. The normal per-run native
report remains separate. Neither checkpoint is proof of completed game startup.

This is a code-size/loading experiment, not a measured performance improvement
or a confirmed fix for the earlier restarts. Console launch remains unverified;
no menu, renderer or gameplay is included in this diagnostic. Private evidence
is retained in `work/ps5/compact-module-verification.json`,
`compact-source-comparison.json`, `compact-host-test.log`, and
`game-native-compact-manifest.json`.

The compact executable is installed in PPSA98732 and verified by complete
SHA-256 readback (`work/ps5/game-native-compact-install.json`). The prior working
picker executable is retained for restoration. The transfer helper and its
temporary payload slot were closed and removed before the manual launch test.

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
