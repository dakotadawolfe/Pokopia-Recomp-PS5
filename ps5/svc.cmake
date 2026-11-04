# SPDX-License-Identifier: GPL-3.0-or-later
set(svc_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
include("${CMAKE_CURRENT_LIST_DIR}/romfs.cmake")
add_library(nxrt_ps5_svc STATIC
    "${svc_root}/src/kernel/svc.cpp" "${svc_root}/src/kernel/svc_thread.cpp"
    "${svc_root}/src/kernel/svc_memory.cpp" "${svc_root}/src/kernel/svc_memory_budget.cpp"
    "${svc_root}/src/kernel/svc_info.cpp" "${svc_root}/src/services/title_defaults.cpp"
    "${svc_root}/src/kernel/svc_ipc.cpp" "${svc_root}/src/ipc/hipc.cpp"
    "${svc_root}/src/ipc/ipc.cpp" "${svc_root}/src/ipc/sm.cpp" "${svc_root}/src/ipc/stub.cpp"
    "${svc_root}/src/services/registry.cpp" "${svc_root}/src/services/logfault.cpp"
    "${svc_root}/src/services/am_ps5.cpp" "${svc_root}/src/services/aoc_ps5.cpp"
    "${svc_root}/src/services/pctl_ps5.cpp" "${svc_root}/src/services/fs_ps5.cpp")
target_compile_definitions(nxrt_ps5_svc PRIVATE
    NXRT_DISABLE_FRAME_TRACE=1 NXRT_DISABLE_TEST_TOOLING=1)
target_compile_options(nxrt_ps5_svc PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(nxrt_ps5_svc PUBLIC nxrt_ps5_kernel nxrt_ps5_romfs)
# PS5 memory handlers currently cover heap size, protection, attributes and
# queries and exclusive stack mapping. Unported calls keep the dispatcher's explicit not-implemented result.
