# SPDX-License-Identifier: GPL-3.0-or-later
set(core_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_core STATIC
    "${core_root}/src/core/dispatch.cpp" "${core_root}/src/core/abi_glue.cpp"
    "${core_root}/src/core/exclusive_diagnostics.cpp"
    "${core_root}/src/core/nxmodule.cpp" "${core_root}/src/core/hooks.cpp"
    "${core_root}/src/loader/module.cpp" "${core_root}/src/loader/nso.cpp")
target_include_directories(nxrt_ps5_core PUBLIC "${core_root}/include"
    "${CMAKE_CURRENT_LIST_DIR}/../recomp/include")
target_compile_options(nxrt_ps5_core PRIVATE -mcx16 -fno-strict-aliasing)
# The Windows frame recorder and stack sampler are not ported yet. The ordinary
# HLE call log stays available; do not link placeholder tracing implementations.
target_compile_definitions(nxrt_ps5_core PRIVATE NXRT_DISABLE_FRAME_TRACE=1)
target_link_libraries(nxrt_ps5_core PUBLIC nxrt_ps5_platform)
# Guest threads and wait queues are in the kernel component; SVC/services remain.
