#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Link the isolated diagnostic with the pinned RADV native platform recipe.
set -euo pipefail
reference=${1:?RADV source checkout required}
platform_sdk=${2:?RADV platform SDK required}
archive=${3:?RADV archive required}
expected_archive=${4:?Expected RADV hash required}
expected_platform=${5:?Expected platform hash required}
output=$(realpath -m "${6:?Output required}")
shift 6
work="$(dirname "$output")/graphics-link"
mkdir -p "$work"
[[ $(sha256sum "$archive" | cut -d' ' -f1) == "$expected_archive" ]] || {
    echo 'RADV archive changed since configuration.' >&2; exit 2;
}
[[ $(sha256sum "$platform_sdk/target/lib/libps5platform.a" | cut -d' ' -f1) == "$expected_platform" ]] || {
    echo 'RADV platform archive changed since configuration.' >&2; exit 2;
}
export PS5_CLANG=${PS5_CLANG:-clang-18}
source "$reference/tools/radv-link.sh"
# The recipe writes its local-symbol map below its first argument. Supplying
# our build directory keeps the reference checkout untouched.
radv_link_recipe "$work" "$platform_sdk" "$archive"
private_platform="$work/libps5platform-graphics.a"
cp "$platform_sdk/target/lib/libps5platform.a" "$private_platform"
objcopy=$(command -v llvm-objcopy-18 || command -v llvm-objcopy)
"$objcopy" --redefine-sym __wrap_pthread_create=pkp_radv_pthread_create "$private_platform"
for i in "${!radv_link_inputs[@]}"; do
    [[ ${radv_link_inputs[i]} != "$platform_sdk/target/lib/libps5platform.a" ]] || radv_link_inputs[i]=$private_platform
done
flags=()
for flag in "${radv_link_flags[@]}"; do
    # app/thread_exit.cpp owns C++ TLS, including main-thread cleanup before
    # the shell exit. RADV's platform owns stack allocation, join, and detach.
    [[ $flag == --defsym=__cxa_thread_atexit_impl=* ]] || flags+=("$flag")
done
inputs=()
for argument in "$@"; do
    case "$argument" in
        -Xlinker|-nostartfiles|-nodefaultlibs|-pthread|-lpthread|-ldl|-lrt|-lm) ;;
        -Wl,*) IFS=, read -r -a parts <<< "${argument#-Wl,}"; inputs+=("${parts[@]}") ;;
        *) inputs+=("$argument") ;;
    esac
done
: "${PS5_PAYLOAD_SDK:?Native application SDK is required}"
"$PS5_PAYLOAD_SDK/bin/prospero-lld" "${flags[@]}" \
    --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 --defsym=__dlclose=0 --defsym=__dlerror=0 \
    --gc-sections --error-limit=0 -Map="$output.map" -o "$output" \
    "${inputs[@]}" "${radv_link_inputs[@]}"
