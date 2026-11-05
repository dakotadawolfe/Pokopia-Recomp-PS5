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
sizes; disk availability uses the kernel `_fstatfs` query because the native
platform's `statvfs` compatibility implementation reports synthetic capacity.
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
