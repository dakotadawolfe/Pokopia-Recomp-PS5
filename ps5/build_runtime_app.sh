#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template=$(realpath "${1:?Usage: build_runtime_app.sh BOILERPLATE_CHECKOUT BUILD_DIRECTORY [DECRYPTED_INPUT]}")
output=$(realpath -m "${2:?Build directory required}")
game_input=${3:-}
: "${NX_PYTHON:?Set NX_PYTHON to the host interpreter with recomp requirements}"
revision=dd44bbdc75437332ed22e3ba95126733419ef25a
[[ $(git -C "$template" rev-parse HEAD) == "$revision" ]] || {
    echo "Native app tools must use revision $revision" >&2; exit 2;
}
git -C "$template" diff --quiet HEAD -- tooling tools runtime
[[ $output != / && $output != "$root" && $output != "$template" ]] || exit 2
expected_sdk=$(realpath "$template/.deps/native/ps5-payload-sdk")
[[ -z ${PS5_PAYLOAD_SDK:-} || $(realpath "$PS5_PAYLOAD_SDK") == "$expected_sdk" ]] || {
    echo 'Use the SDK belonging to the pinned native app toolchain.' >&2; exit 2;
}
export PS5_PAYLOAD_SDK="$expected_sdk"
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || exit 2
test -x "$PS5_PAYLOAD_SDK/bin/prospero-clang++"
test -f "$template/runtime/libc.prx" || bash "$template/tools/rebuild-libc.sh"
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
tool="$template/build/host/ps5-native-tool"
bash "$template/tools/build-host-tools.sh"
cmake -S "$root/ps5/tests" -B "$output" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DNX_PYTHON="$NX_PYTHON" -DPKP_BUILD_NATIVE_APP=ON -DPKP_NATIVE_APP_ROOT="$template" \
    -DPKP_GAME_IMAGE_INPUT="$game_input"
cmake --build "$output" --target ps5_runtime_app --parallel "$jobs"
"$tool" link --in "$output/ps5_runtime_app" --out "$output/eboot.elf" \
    --stub-dir "$PS5_PAYLOAD_SDK/target/lib" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$NX_PYTHON" "$root/ps5/package_runtime_app.py" "$output" "$template" "$tool"
