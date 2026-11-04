#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template=$(realpath "${1:?Usage: build_startup_app.sh NATIVE_ROOT BUILD_DIR DECRYPTED_INPUT MODULE_BUILD}")
output=$(realpath -m "${2:?Separate startup build directory required}")
input=$(realpath "${3:?Decrypted input required}")
modules=$(realpath "${4:?Verified production module build required}")
optimization=$(sed -n 's/^NX_POSIX_OPT:STRING=//p' "$modules/CMakeCache.txt")
optimization=${optimization:--O1}
[[ $optimization =~ ^-O[0123sz]$ ]] || exit 2
: "${NX_PYTHON:?Set NX_PYTHON to the interpreter with recomp requirements}"
revision=dd44bbdc75437332ed22e3ba95126733419ef25a
[[ $(git -C "$template" rev-parse HEAD) == "$revision" ]] || exit 2
git -C "$template" diff --quiet HEAD -- tooling tools runtime
[[ $output != / && $output != "$root" && $output != "$template" && $output != "$modules" && $output != "$input" ]] || exit 2
if [[ -f $output/CMakeCache.txt ]]; then
    grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root/ps5/tests" "$output/CMakeCache.txt" || exit 2
fi
expected_sdk=$(realpath "$template/.deps/native/ps5-payload-sdk")
[[ -z ${PS5_PAYLOAD_SDK:-} || $(realpath "$PS5_PAYLOAD_SDK") == "$expected_sdk" ]] || exit 2
export PS5_PAYLOAD_SDK="$expected_sdk"
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || exit 2
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
tool="$template/build/host/ps5-native-tool"
test -x "$tool"
cmake -S "$root/ps5/tests" -B "$output" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DNX_PYTHON="$NX_PYTHON" -DNX_POSIX_OPT="$optimization" -DPKP_NATIVE_APP_ROOT="$template" -DPKP_BUILD_NATIVE_APP=OFF \
    -DPKP_GAME_IMAGE_INPUT="$input" -DPKP_TEST_RTLD=ON -DPKP_TEST_SDK=ON \
    -DPKP_SDK_MODULE_BUILD="$modules" -DPKP_TEST_GAME_STARTUP=ON -DPKP_BUILD_NATIVE_GAME_STARTUP=ON \
    -DPKP_NATIVE_STARTUP_HEAP_LIMIT_MIB=512
cmake --build "$output" --target ps5_game_startup_app --parallel "$jobs"
"$tool" link --in "$output/ps5_game_startup_app" --out "$output/startup.elf" \
    --stub-dir "$PS5_PAYLOAD_SDK/target/lib" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$output/startup.elf" --out "$output/startup.bin" --magic 0x1D3D154F
"$NX_PYTHON" -B "$root/ps5/package_startup_app.py" "$output" "$template" "$tool"
