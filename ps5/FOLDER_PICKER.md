# Native game folder selection

`ps5/app/folder_picker.*` provides the controller-driven launch picker. The
graphics runtime diagnostic draws its view using the same real Vulkan
Device/Renderer that subsequently draws the test scene. The picker does not open
VideoOut, a browser or a terminal. Its interface is renderer-neutral so the game
launcher can keep the same surface after selection.

The user browses `/data` or an available `/mnt/usb0` through `/mnt/usb7`, enters
the existing extracted game folder and selects **Use this folder**. Cross opens
or confirms, Circle goes back, D-pad/left stick moves, and Square refreshes
storage. Circle at the storage list cancels normally. Disconnected controllers,
missing drives, packages and unreadable folders remain visible states; they do
not silently close the application. A held Cross from app launch cannot confirm
a remembered folder until released and pressed again.

Selection supports `exefs` and `romfs` beside each other, a `base` tree, and
`updates/<version>` trees. Selecting the top game directory searches matching
updates before its base tree; entering a specific update preserves that choice.
The chosen update's base and update RomFS directories are recorded in order.
This records locations only; it does not implement merging or copy either layer.
Entering `exefs` or `romfs` directly also resolves the surrounding layout.

The picker performs quick, read-only checks:

- All four executable files are readable NSOs with bounded file segments.
- `main` carries the supported build ID from the current release; other builds
  receive an explicit unsupported message.
- `main.npdm` is readable and has its metadata header.
- At least one nonempty, readable asset exists in the selected RomFS layers.

These are header, build-ID and presence checks, **not full executable hashes or
complete asset verification**. Full module hash validation is required before
eventual game startup. The picker does not decompress modules, validate every
asset, request keys or firmware, or claim a chosen update is self-contained.
NSP/NSZ/XCI/XCZ/NCA files are shown as packages and cannot be selected as extracted
game folders.

Only `/data/pokopia-recomp/game-location.txt` is saved, containing the selected
folder path. A bounded read validates remembered text; writing uses a new
exclusive temporary file, flush, close and atomic rename. Failure leaves the
previous setting intact. Before confirmation, the folder is checked again so
removing a USB drive while the UI is open cannot save a stale selection. No game
file is created, copied, renamed, moved or deleted by the picker. A remembered
folder is presented for confirmation or browsing on the next launch.

## Integration

```cpp
nxrt::app::FolderPicker picker;
while (!picker.view().done && !picker.view().cancelled) {
    picker.update();
    // Render picker.view() with the application's existing renderer.
}
// picker.selection(): root, exefs, build_id, ordered romfs_layers.
```

The view contains title, location, status, controls, at most 12 visible rows and
a selected index relative to those rows. The renderer must not apply a second
scroll offset. `update(PickerInput, milliseconds)` permits deterministic host
tests without a controller or console.

Compile `folder_picker.cpp`, `folder_picker_paths.cpp` and
`folder_picker_pad.cpp` with `NXRT_TARGET_PS5=1`; link `libScePad` and
`libSceUserService`. `ps5/picker.cmake` supplies this as `nxrt_ps5_picker`.
Native polling reads the signed-in user's pad, borrows an existing handle or
opens one, and closes only a handle it opened. It uses no process privilege
changes or virtual controller injection. Library initialization and acquired
handle information are recorded once to support the first physical test.

## Focused validation

From the repository root in Linux/WSL:

```sh
cmake -S ps5/tests/picker_suite -B /path/to/picker-host -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Debug
cmake --build /path/to/picker-host
ctest --test-dir /path/to/picker-host --output-on-failure

export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
cmake -S ps5/tests/picker_suite -B /path/to/picker-ps5 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/picker-ps5
```

As of 2026-11-04, all **8 host cases pass**, covering supported layouts, update
ordering, invalid files/builds, atomic bounded configuration, remembered/missing
storage messages, unplug-before-confirmation, package status, cancellation,
scroll/repeat and confirmation debounce. Synthetic test directories are removed
after each case. The PS5 standalone target compiles and links. Production file
operations `open`, `fstat`, `write`, `fsync`, `close`, `rename`, `unlink`, `mkdir`
and `stat` are native `libkernel` imports. Import presence alone does not prove
an operation is allowed in native application context.

The integrated native picker/renderer package is built separately by the graphics
target. Selecting a folder currently leads to the renderer test scene; it does
not start Pokopia.

## Native storage-path correction

The first native launch displayed the picker, initialized the signed-in user's
pad and cancelled cleanly, but reported no accessible storage. Report
`graphics-runtime-1793792739-419.log` records that run. The application imported
`realpath` from the native libc shim. The pinned boilerplate's
`libc_builder.cpp` maps ordinary function exports to its zero-return fallback;
`std::filesystem::canonical` therefore could not resolve any root. The same
operation worked in the payload context, which uses a different libc path.

The first correction used `lstat`. It passed eight cases in payload context
(`platform-1793793311-429.log`) but still failed inside the native application
(`graphics-runtime-1793794102-440.log`). A native audit then established that
`lstat` returns `EPERM` for every tested path while `stat` and `opendir` work
for `/data` and `/mnt/usb0`. Payload success did not cover this restriction.

The picker now normalizes absolute paths and walks each component with
`open(O_DIRECTORY | O_NOFOLLOW)` followed by `fstat`. NSO and asset checks use
opened descriptors too; asset traversal no longer relies on recursive
`symlink_status` calls. Real directories and storage boundaries remain required;
no privileges or mounts are changed. Host tests simulate both a null-returning
`realpath` and denied `lstat` and retain missing-path, parent and symlink cases.

Native report `graphics-runtime-1793794634-461.log` confirms that production
path checks recognize `/data` and USB0 and reject absent USB1 through USB7.
In the same native process, a synthetic extracted game passes layout validation,
picker navigation/confirmation, configuration save and remembered-location
reload. Its seven temporary files and four directories are removed, followed by
`PICKER_AUDIT_END native=true status=PASS` and normal application exit. All eight
host cases also pass with the restricted-API simulation.

The installed executable is 19,652,449 bytes, SHA-256
`e9ea79967824e29e6227b5267e5404519ad39407f61d724bb47bdc293462295d`.
The diagnostic trigger was removed and its absence verified. Selecting the
user's actual game folder and running the subsequent renderer scene still need
the next controller test; no real game assets were modified or selected.

## Repeating the native audit

The diagnostic app runs a bounded storage audit instead of opening the picker
only when `/data/pokopia-recomp/tests/picker-storage-audit` contains exactly
`PICKER_STORAGE_AUDIT` followed by a newline. The audit creates only its own
`picker-audit-<pid>` fixture under the tests directory and refuses an existing
fixture. Always remove the trigger after collecting the native report.

`launch_graphics_app.c` launches only installed title `PPSA98732` and refuses
when another application is running. Build it locally with the payload SDK:

```sh
"$PS5_PAYLOAD_SDK/bin/prospero-clang" ps5/launch_graphics_app.c \
  -lSceSystemService -lSceUserService -o /path/to/private/launch-graphics-app.elf
```

Run the helper with `console_probe.py`, then collect `graphics-runtime-*.log`.
The launcher exiting does not establish native-app success: check the app's
own final audit marker, clean exit and process disappearance independently.
