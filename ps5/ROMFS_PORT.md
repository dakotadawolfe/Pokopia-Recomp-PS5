# Read-only RomFS port

`fs_romfs.cpp` now builds the existing RomFS layout from a POSIX directory and
reads file data through cached, positioned `pread` calls. The Windows path,
handle and write-watch code remains selected by `_WIN32`. Names retain their
UTF-8 bytes; invalid names and non-file/directory entries fail indexing. Missing
or truncated backing files return errors, not replacement zero data.

POSIX metadata scanning uses `open(O_NOFOLLOW | O_NONBLOCK)` and `fstat` because
native PS5 apps refuse `lstat`. This retains link and special-file rejection.
The host suite passes 17 cases, including building and reading a nested image
while `lstat` returns `EPERM`. Native applications also need the directory stream
bindings used by the graphics app; ordinary system `opendir` imports do not cover
that environment. See [native startup integration](GAME_STARTUP_TRACE.md#native-directory-integration)
for the current build and console evidence.

Native report `game-native-1793820127-95.log` now confirms successful metadata
construction from the user's selected USB folder: 44 files, eight directories,
7,246,055,230 bytes of file extents. The files include `.listing` sidecars.
The trace then reaches the game's main-module startup and stops at
its diagnostic heap limit. This validates native discovery/indexing, not full
asset reads by the game or gameplay.

`fs_ps5.cpp` registers `fsp-srv`. `SetCurrentProcess` binds a session to the real
runtime process. Commands 200 and 205 (index zero only) return read-only
`IStorage` objects backed by that RomFS. Reads check argument length, signed
ranges, descriptor capacity and guest write permissions; `GetSize` returns the
image size. Flush succeeds because this backing has no pending writes. Range
queries report unencrypted storage without speed emulation; invalidation drops
cached host handles while active readers retain their own references. Writes,
resize, fill-zero and signature destruction fail read-only. Other operations, including the
separate `IFileSystem` interface and all save operations, fail unsupported even
when the generic IPC stub-result setting is zero. Reset creates fresh service
state without invalidating existing storage objects.

The filesystem startup policy reports access logging disabled (command 1005).
Command 1004 accepts disabling logging, rejects unsupported log destinations,
and rejects invalid mode bits. Command 1003 acknowledges the existing fixed
policy that never creates saves automatically. Command 1011 reports program
index zero and a count of one, matching the mounted-program restriction. These
queries require a process-bound session and reject unexpected resources.

The command 1011 layout follows the
[Atmosphere filesystem proxy interface](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/fssrv/sf/fssrv_sf_i_file_system_proxy.hpp)
and the inherited core. The current libnx helper sends 1007 for that named
query, whereas the Atmosphere interface assigns 1007 to update-partition
registration; the PS5 port does not alias the two commands. Storage command 5
uses the 24-byte input and 64-byte output from the documented IPC interface.
[Operation IDs](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/fs/fs_operate_range.hpp)
2 and 3 select invalidation and query respectively;
[range information](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/fs/fs_query_range.hpp)
contains AES/speed flags and reserved bytes. Queries validate the image range.
Invalidation accepts the full-cache `(0, INT64_MAX)` request, releases the
entire handle cache, and leaves the indexed metadata immutable.

The source path is `[fs] romfs` / `NXRT_FS_ROMFS`, defaulting to
`/data/pokopia-recomp/game/romfs`. Configure it before opening the service and
reset FS state after changing it. This implementation consumes one already
prepared tree; it does not merge update layers or stage files. Game RomFS assets
have not been staged or read by the game as part of this work. Fixture success
does not establish game startup, rendering, or gameplay.

Wire layouts were checked against [libnx fs.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/fs.c)
and [Switchbrew's filesystem service research](https://switchbrew.org/wiki/Filesystem_services#OpenDataStorageWithProgramIndex).

## Local builds

Run from the repository root in a Linux/WSL shell. Set paths for the local SDK
and a writable build directory; no game files or network build service are used.

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
build_root=/path/to/local-build

cmake -S ps5/tests/romfs_suite -B "$build_root/romfs-host" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_root/romfs-host"
ctest --test-dir "$build_root/romfs-host" --output-on-failure

cmake -S ps5/tests/romfs_suite -B "$build_root/romfs-ps5" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_root/romfs-ps5"

cmake -S ps5/tests/fs_suite -B "$build_root/fs-ps5" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_root/fs-ps5"
```

The outputs are `ps5_romfs_tests` and `ps5_fs_tests` ELF diagnostics. The FS suite
requires the PS5 guest-memory backend. The same twelve service cases also run
through the combined SVC/native test targets. Tests use private temporary trees;
console reports remain under `/data/pokopia-recomp/tests/`.

## API audit and validation boundaries

The compiled reader imports `open`, `pread`, `close`, `fstat`, `stat`, and `lstat`
from `libkernel`, and directory enumeration from `libSceLibcInternal`. The SDK's
libc++ directory implementations use those imports. They do not use the raw
`fstatat` stub. A focused case checks real missing-file/invalid-descriptor
operations for the POSIX `-1` and `errno` contract.

The SDK archive's `symlink` and `mkfifo` functions are raw syscall wrappers that
do not translate FreeBSD carry/error returns to POSIX errno. On this console,
the link fixture returned `symlink=45 errno=0`; independent `lstat` returned
`-1 errno=2 mode=0`, proving no link was created. See the
private console report `romfs-runtime-1793788597-364.log`.
The earlier libc++ helper treated the positive error as success. That fixture
failure is not a passed console link-rejection test. Link/FIFO rejection remains
host-only coverage, as does the sparse 5 GiB fixture because console sparse-file
allocation has not been established. Production indexing still rejects both
entry kinds when encountered.

Validation as of 2026-11-04:

- Host reader suite: **16 cases, 0 failures**, including the independent RomFS
  parser, UTF-8 paths, 4 GiB boundary reads, cache eviction under concurrent
  reads, changed files, and host link/FIFO rejection.
- PS5 reader: **14 cases, 0 failures** in `romfs-runtime-1793788861-368.log`,
  with process exit verified. This includes the native file-error contract.
- PS5 filesystem service: all **8 cases passed** within the combined **99-case,
  zero-failure** SVC run `svc-runtime-1793788869-371.log`, with process exit
  verified. Combined ELF SHA-256:
  `82fe0d0b811851ed6fea8dbbfc1c9601ce6a2a89a9877b1a43dae3af08c594d9`.
- No game-asset staging, save writes, key handling, or gameplay proof.

Final locally built diagnostic artifacts (not repository inputs):

| Artifact | SHA-256 |
| --- | --- |
| Reader ELF | `0e884e7c4bec9165422d9d92adbe64f2d0c99eb83a11bfc126b36951e9d8d6fa` |
| FS service ELF (build only; cases ran in the combined SVC ELF) | `16ec182ff56959bfc12ba15d9d285a4f0df8a665b096e26f4749cf6f09748e6f` |
