# Graphics supervisor bootstrap

## Observed native boundary

`game-native-1793821562-97.log` ran all 38 exception cases inside the native
application with zero failures. It registered all game modules, allocated the
full 10.36 GiB heap in 714 ms, returned from `nninitStartup` and completed SDK
static initialization (`SDK_CALLBACK returned=1`). Its next request was
`ISelfController` command 2000, which the PS5 application service had not
implemented. The diagnostic stopped with result `0x000bcbd5` and requested
shell exit with status 2. The user observed Home. Game exception counters were
all zero; the synthetic tests exercised throws, but this game path did not.

Upstream `runtime/src/services/am.cpp` returns a moved `StubService` session for
this command. That establishes the handle boundary, not a graphics backend.
The PS5 implementation now returns a process-owned `GraphicsSupervisor` session.
It remains a separate handle when the caller is an AM domain object.

## Supported lifecycle and evidence

The public [GetGraphicsSupervisor documentation](https://switchbrew.org/wiki/Applet_Manager_services#GetGraphicsSupervisor)
identifies the returned handle as TIPC. The
[GSV user-session documentation](https://switchbrew.org/wiki/Switch_2:_GSV_services#UserSession)
describes remote boot, process binding and context creation as commands 4, 11
and 101, respectively. Their input layouts are corroborated by the supplied
SDK's generated implementation at `0x7500412630`, reached from
`nn::gsv::BootWithRemoteMemory` at `0x7500412580`. SDK SHA-256:
`7c12078e9f853f5d77f395155f7308f264fa6e93fadaeba8dca525f4483c09b2`.

| Command | Accepted inputs | State change |
| --- | --- | --- |
| 4: BootWithRemoteMemory | One copied owning-process handle; 48 bytes containing resource user ID, version `0x8000000000000005`, three zero reserved words and zero existing remote usage | Records the validated remote-boot configuration |
| 11: SetCurrentProcessHandle | One copied owning-process handle; no raw data | Retains the process object after temporary IPC handles are released |
| 101: CreateContext | No handles or raw data | Creates a context retaining the bound process and boot configuration |

Commands must use TIPC and follow that order. Duplicate initialization,
mismatched identity, extra handles, wrong sizes, buffer descriptors, unsupported
versions and nonzero existing remote usage fail without partially advancing
state. Sessions obtained from the same AM application share this context;
resetting AM creates a new state without invalidating existing handles.

This is application-session bookkeeping. It does not create GPU resources,
reserve or donate remote memory, expose syncpoint/display/GPU sessions, or submit
commands. Transfer-memory boot and all remaining operations explicitly fail.
In particular, returning the AM handle does not claim renderer availability.
The static SDK startup chain continues into nvsrv, syncpoint, native-memory and
frame-management initialization; those dependencies still need implementation.

## Diagnostic coverage

Startup now validates both CMIF and TIPC result words, including requests through
the explicit user buffer. TIPC replies use message type zero but place their
result directly after the descriptors. The diagnostic preserves the request's
protocol when parsing them; it does not apply CMIF alignment or read a stale
result word. A failed or malformed reply stops before the SDK enters its fatal
path. Request reports include protocol, command and payload/handle counts.

Build the focused service suite from a configured PS5 test directory:

```sh
cmake --build /path/to/ps5-test-build --target ps5_ipc_tests --parallel 4
```

Console report `ipc-runtime-1793821959-103.log` records **43 cases, zero failures**.
Eight new cases exercise domain/session transfer, process retention, lifecycle
ordering, invalid inputs, reset isolation, explicit unsupported operations and
CMIF/domain/TIPC error decoding. Payload SHA-256:
`3d394d6e8933c5f9d8f29f5d3d3a2faebd6d679dd0371e1fded7711fbef4b290`.
The native startup target and earlier bounded loader/SDK targets compile and
link. This payload result does not establish native game graphics behavior.

The native candidate is 257,927,571 bytes, SHA-256
`7bc505fef0f2cccaff320a42ba39c9f33ef9ae842267447a85d34ea2e9e018db`.
It retains the same saved folder, 11 GiB heap threshold, 1 GiB allocation
headroom check, exception suite and finite startup limits. Native progress
beyond the graphics-session request awaits the next manual launch. No game
assets are copied, and the renderer/menu remain absent from this diagnostic.

The candidate was installed as `PPSA98732` with the application stopped and
unmounted. Complete SHA-256 readback matched the candidate hash above; the
previous executable was backed up before replacement. The owned FTP helper
and its payload slots were removed after installation, and its port closed.
The application has not been launched automatically.
