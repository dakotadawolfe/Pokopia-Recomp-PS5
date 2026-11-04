# Read-only time service

`time_ps5.cpp` registers `time:u` and implements the ordinary system clocks,
steady clock, UTC timezone conversions and shared-clock handle. The existing
Windows `time.cpp` remains unchanged. `time_clock.cpp` contains the portable
clock model; `timezone.cpp` retains its existing Gregorian and supplied-rule
DST conversion, with validation before caller-controlled transition arithmetic.

Host UTC seeds the system clocks once. Guest steady time starts at 3600 seconds
and advances from `kernel::system_tick()` at `kernel::tick_hz()`. Local, user and
network clock reads currently share that anchor; there is no network clock
synchronization. The initial UTC value has one-second precision. Later host wall
clock changes do not move guest steady time or silently replace its source ID.
Reset creates a fresh source, while existing service objects retain their clock.

Timezone policy is explicitly UTC. The service lists and loads only `UTC`, but
calendar conversion with a complete caller-supplied rule supports its valid
offsets and DST transitions. No firmware timezone database or host timezone is
read. All device-calendar timestamps are UTC until a separate timezone policy
is implemented.

Writes return permission denied. Administrative service names are not registered.
Unknown commands return unsupported even if generic IPC stubs are configured to
return success. Clock snapshots, timezone database version, RTC-reset detection
and timezone changes remain unsupported. Clock and timezone events have real
kernel ownership and remain unsignaled because these read-only contexts do not
change. A reset does not fabricate a change event on the old context.

## Shared clock contract

The 0x1000-byte guest object is backed by `shared_memory_create`, with a stable
private writable alias and read-only guest mapping permission. Closing one
copied handle does not destroy another handle or a live mapping. The service
publishes its immutable image before returning a handle, using counter 1 and
slot 1 in each double-buffered block.

| Offset | Value | Slot size | Padding after 32-bit counter |
| --- | --- | --- | --- |
| `0x000` | Steady base in nanoseconds and source ID | `0x18` | 4 |
| `0x038` | Local system clock context | `0x20` | 4 |
| `0x080` | Network system clock context | `0x20` | 4 |
| `0x0C8` | Automatic correction disabled | 1 | 0 |
| `0x0D0` | Continuous-adjustment clock context | `0x38` | 4 |

The older steady fast path uses `(base_ns + ticks_ns) / 1e9`. Here `base_ns` is
`3600e9 - initial_ticks_ns`, including any fractional initial second. A system
clock adds its context offset, `initial_utc_seconds - 3600`.

The current supplied SDK also uses `0x0D0`. Its signed correction is the initial
offset plus the elapsed nanoseconds times a rate, shifted by the divisor; that
result is clamped toward a target offset according to the rate's sign. The port
sets the rate and divisor to zero and both offsets to `base_ns`. Thus this path
and the classic path agree, including when the host has run longer than 3600
seconds and `base_ns` is negative. The two offsets in this block are nanoseconds;
they are not the seconds fields of a system clock context.

Static consumer verification used the private generated SDK for build ID
`cc7da00d672399efe7958790d17fae396b1fde9f`: steady reader `0x7500557160`, system
context conversion `0x75005572C0`, tick reader `0x7500148D50`, and tick conversion
`0x750014A2C8`. The conversion uses 31,250,000 Hz and 32 ns per tick, consistent
with the runtime's Switch 2 tick frequency. The portable tests additionally
exercise 19,200,000 Hz. No generated game code is included in this port or its
tests. The older layout and clock interfaces can also be compared with
[libnx time.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/time.c).

## Local builds and integration

Run from the repository root in Linux/WSL, selecting local SDK and build paths:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
build_root=/path/to/local-build

cmake -S ps5/tests/time_suite -B "$build_root/time-host" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_root/time-host"
ctest --test-dir "$build_root/time-host" --output-on-failure

cmake -S ps5/tests/time_suite -B "$build_root/time-ps5" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_root/time-ps5"
```

The executable is `ps5_time_tests`. Host builds run six portable clock/timezone
cases. PS5 builds add six service cases and use the actual guest-memory,
shared-memory, event and IPC implementations. Test setup and teardown release
all shared views before shutting down guest memory, including after an assertion
failure. The tests create no files or game-asset fixtures.

`ps5/svc.cmake` includes `time.cmake`, compiles `time_ps5.cpp`, and links
`nxrt_ps5_time_support`. Startup calls `time_reset()` and `register_time()`.
The combined SVC/native suite includes `test_time_clock.cpp`, `test_time_zone.cpp`
and `test_time_ps5.cpp` (12 cases). These test and runtime inputs have no dependency
on the private generated SDK.

## Validation

As of 2026-11-04:

- Host: **6 cases, 0 failures**. Coverage includes both tick frequencies,
  fractional origins, positive/negative steady bases, both shared consumer
  formulas, UTC/Gregorian date oracles, signed-16-bit year endpoints, a DST gap
  and fold, malformed rules and integer limits.
- PS5: all **12 time cases passed** within the combined **117-case, zero-failure**
  SVC run `svc-runtime-1793790497-394.log`. Service cases cover clock/source
  agreement, real shared mappings and permissions, handle/reset ownership,
  denied operations, supplied-rule conversion and guest-buffer bounds/permissions.
  Combined ELF SHA-256:
  `abd77a25c0643c8749b8291fd2cf692d8c3d67e3fd49a464243bdbb24406d499`.
- Final local standalone ELF SHA-256 (built, not separately launched on console):
  `c01056fae7c4832483bff6207ef74cbdf914c3738d605808c62e28ee5e0ab38e`.
  Build path for this run:
  `/root/pokopia-recomp-ps5-build/time-port-ps5/ps5_time_tests`.
- Actual SDK trace `sdk-startup-1793790561-395.log` obtained and mapped the clock
  successfully at calls 135-139, then reached the separate external-call boundary
  `nninitStartup` (`0x7500092790`, return address `0x7500118EDC`). This advances
  beyond time service initialization; it does not establish full SDK startup,
  game clock use, rendering or gameplay.

The earlier trace `sdk-startup-1793790049-391.log` reached the real `time:u`
service and obtained its shared handle, then aborted because a trailing guest
guard-space query reported host-page padding as inaccessible rather than free.
That is a separate shared-memory mapping issue. It does not count as a passed
time startup run; see [shared-memory port](SHARED_MEMORY_PORT.md).
