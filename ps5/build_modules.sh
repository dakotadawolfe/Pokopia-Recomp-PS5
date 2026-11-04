#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to the native SDK directory}"
: "${NX_PYTHON:?Set NX_PYTHON to the host interpreter with recomp requirements}"
input=${1:?Usage: build_modules.sh DECRYPTED_INPUT BUILD_DIRECTORY}
output=${2:?Build directory required}
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive' >&2; exit 2; }
cmake -S "$root/ps5" -B "$output" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DNX_PYTHON="$NX_PYTHON" -DPKP_INPUT="$input"
cmake --build "$output" --target pkp_native_components --parallel "$jobs"
"$NX_PYTHON" "$root/ps5/check_modules.py" "$output" \
    --ar "$PS5_PAYLOAD_SDK/bin/prospero-ar"
