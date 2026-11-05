# Account-save bring-up

The manual run recorded in private report `game-native-1793899403-101.log`
passes the offline display bridge, then deliberately stops at
`IApplicationFunctions` command 20 (`EnsureSaveData`). Main returns status 2
and requests a normal shell exit. This report does not establish a game menu
or gameplay; one renderer presentation is not evidence of a visible game frame.

## Current backend

`fs_save_ps5.cpp` implements account-save creation, mounting, size queries and
extension. Files and directories use the shared `fs_volume.cpp` journal with
POSIX positioned I/O, copy on write and a replayable commit decision. The
filesystem supports creation, deletion, rename, directory enumeration,
timestamps, free/total quota queries, file read/write/resize/flush and commit.
Unsupported commands keep explicit errors.

The development root is `/download0/pokopia-saves/<program>/<account>`, with
account metadata and a `data` directory below it. It is separate from the
selected USB inputs. `[fs] save_dir` supplies isolated test roots. No existing
source save is imported or overwritten.

The supplied extracted folder contains ExeFS and RomFS but no control NACP.
The private startup target therefore declares a **development policy** of
64 MiB data and 64 MiB journal, with extension limited to 96 MiB each. These
are not recovered game metadata values. Growth checks use actual visible file
sizes. Disk availability is advisory: `_fstatfs` supplies a measurement where
permitted, but the native sandbox denies it with `EPERM`. That denial is
reported as unknown space, while actual create/write/flush/commit failures
still fail. A measured full disk and other query errors still stop creation
or extension. The native platform's synthetic `statvfs` capacity is not used.
The 256 MiB download allocation can hold two maximum-size data generations
plus metadata; no additional journal-sized file is allocated.

Only the current application's fixed offline account can mount this backend.
Read-only sessions share the existing transaction without recreating its work
directory. Commit refuses open writable files, so a surviving writable handle
cannot mutate a file after it has become committed. Closing the last volume
without commit discards its pending changes. A decided but interrupted commit
is replayed on the next mount. Invalid recovery paths retain the marker and
fail instead of redirecting writes outside the volume.

Expected first-boot missing-file and already-existing-directory replies reach
the SDK unchanged. The diagnostic still stops on malformed requests, actual
I/O failures and unported commands.

## Validation

- Fourteen host volume cases pass, covering ordinary operations, rollback,
  commit recovery before and during replay, and corrupt journal paths.
- Thirty-six filesystem, account-save and application-service cases pass in
  PS5 payload report `fs-runtime-1793901974-106.log`. This includes the applet
  request that stopped the game, persistent extension, quotas, account checks,
  guest buffer validation and missing-data protection.
- The native game app runs a small `/download0` write/commit/reopen/recovery
  check before startup and removes its synthetic fixture. It also verifies
  that a positive 20 ms thread sleep really waits. Failure prevents the game
  from starting. These native checks still need the next manual launch;
  payload results do not prove the app's filesystem permissions.

### Native free-space query correction

The next manual launches exited before game startup. Private report
`game-native-1793902536-107.log` identifies the new preflight as the cause:
the temporary `/download0` directory was created, but `_fstatfs` returned
`EPERM`. The check incorrectly treated an unavailable advisory measurement
as a failed storage operation. The native 20 ms sleep test passed at 20,091 us;
positive sleeps are not returning immediately in this app.

The corrected preflight permits an explicitly unavailable capacity query and
continues to the actual write, commit and recovery checks. Save creation and
extension follow the same policy. No disk capacity is invented and no actual
I/O failure is ignored. Thirty-nine component cases pass on PS5 in private
report `fs-runtime-1793902774-112.log`, including injected `_fstatfs`/`lstat`
permission errors, commit recovery, extension, quota limits, write failures,
and genuine zero-space/I/O-error replies. The fourteen host volume cases pass.
The native save round trip and game startup still require a manual run.

The prior private app built from `dfa80f1` was installed with full executable readback
verification. Its native executable is 279,846,745 bytes, SHA-256
`5aea9402d735de036d06964706d3a7db5aca994fffba08c8e8205d45472255b8`.
The install receipt, previous executable and verified matching symbol archive
are retained privately. The owned transfer helper and diagnostic payload slots
were removed. No automatic game launch was performed.

Build the standalone checks locally:

```sh
cmake -S ps5/tests/volume_suite -B /path/to/volume-host -G Ninja
cmake --build /path/to/volume-host --parallel 4
ctest --test-dir /path/to/volume-host --output-on-failure
```

For the console, configure `ps5/tests/fs_suite` with the payload SDK toolchain
and `PS5_PAYLOAD_SDK`, then build `ps5_fs_tests`. The suite uses synthetic files
and does not launch the game. Collect `fs-runtime` with `console_probe.py` and
close its owned helper after the session.

## Release boundaries

This is a development save backend. Native PS5 Saved Data integration, selection
of the active console user, save migration and uninstall behavior remain
unverified. A title-owned directory alone does not satisfy the normal Saved
Data UI requirement or prove ShadowMountPlus removes it on uninstall.
The on-console import/recompilation pipeline and verified USB-independent
installation also remain outstanding. See [release requirements](RELEASE_REQUIREMENTS.md).

Only source and synthetic tests are public. Real game files, saves, caches,
generated modules, executables and diagnostic reports remain private.
