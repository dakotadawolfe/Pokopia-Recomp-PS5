#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:?Usage: check_platform.sh BUILD_DIRECTORY}
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to the native SDK directory}"
cmake -S "$root/ps5/tests" -B "$out/host-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$out/host-tests" --parallel 2
ctest --test-dir "$out/host-tests" --output-on-failure
cmake -S "$root/ps5/tests" -B "$out/ps5-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake"
cmake --build "$out/ps5-tests" --parallel 2
echo 'Host tests passed and PS5 tests linked. Collect the console reports to verify this build on hardware.'
