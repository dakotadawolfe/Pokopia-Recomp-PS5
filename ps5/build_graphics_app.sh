#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template=$(realpath "${1:?Usage: build_graphics_app.sh NATIVE_ROOT VULKAN_ROOT RADV_SDK MESA_ROOT RADV_ARCHIVE BUILD_DIR [ISOLATION_MANIFEST]}")
vulkan=$(realpath "${2:?Pinned Vulkan checkout required}")
radv_sdk=$(realpath "${3:?RADV platform SDK required}")
mesa=$(realpath "${4:?Pinned Mesa checkout required}")
archive=$(realpath "${5:?Isolated RADV archive required}")
output=$(realpath -m "${6:?Separate graphics build directory required}")
manifest=$(realpath "${7:-$(dirname "$archive")/manifest.json}")
if [[ $# -ge 7 ]]; then shift 7; else shift 6; fi
[[ $output != / && $output != "$root" && $output != "$template" && $output != "$vulkan" && $output != "$mesa" && $output != "$radv_sdk" ]] || exit 2
if [[ -f $output/CMakeCache.txt ]]; then
    grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root/ps5/tests/graphics_suite" "$output/CMakeCache.txt" || {
        echo 'Use a separate build directory for the graphics diagnostic.' >&2; exit 2;
    }
fi
revision=dd44bbdc75437332ed22e3ba95126733419ef25a
[[ $(git -C "$template" rev-parse HEAD) == "$revision" ]] || {
    echo "Native toolchain must use revision $revision" >&2; exit 2;
}
git -C "$template" diff --quiet HEAD -- tooling tools runtime
expected_sdk=$(realpath "$template/.deps/native/ps5-payload-sdk")
[[ -z ${PS5_PAYLOAD_SDK:-} || $(realpath "$PS5_PAYLOAD_SDK") == "$expected_sdk" ]] || {
    echo 'Use the SDK belonging to the pinned native app toolchain.' >&2; exit 2;
}
export PS5_PAYLOAD_SDK="$expected_sdk"
export PS5_CLANG=${PS5_CLANG:-clang-18}
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || exit 2
python=${NX_PYTHON:-python3}
test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang++"
test -f "$template/runtime/libc.prx" || { echo 'Build and verify the pinned native runtime first.' >&2; exit 2; }
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
tool="$template/build/host/ps5-native-tool"
test -x "$tool" || { echo 'Build the pinned native host tools first.' >&2; exit 2; }
cmake -S "$root/ps5/tests/graphics_suite" -B "$output" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DPKP_NATIVE_APP_ROOT="$template" -DPKP_RADV_SOURCE_ROOT="$vulkan" -DPKP_RADV_SDK_ROOT="$radv_sdk" \
    -DPKP_MESA_SOURCE_ROOT="$mesa" -DPKP_RADV_ARCHIVE="$archive" -DPKP_RADV_MANIFEST="$manifest" "$@"
cmake --build "$output" --target ps5_graphics_app --parallel "$jobs"
"$tool" link --in "$output/ps5_graphics_app" --out "$output/eboot.elf" \
    --stub-dir "$PS5_PAYLOAD_SDK/target/lib" \
    --stub "$output/graphics-stubs/libSceAgc.so" --stub "$output/graphics-stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$python" -B "$root/ps5/package_graphics_app.py" "$output" "$template" "$vulkan" "$mesa" "$tool"
