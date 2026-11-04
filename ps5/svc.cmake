# SPDX-License-Identifier: GPL-3.0-or-later
set(svc_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_svc STATIC
    "${svc_root}/src/kernel/svc.cpp" "${svc_root}/src/kernel/svc_thread.cpp"
    "${svc_root}/src/kernel/svc_memory.cpp" "${svc_root}/src/kernel/svc_memory_budget.cpp"
    "${svc_root}/src/kernel/svc_info.cpp" "${svc_root}/src/services/title_defaults.cpp")
target_compile_definitions(nxrt_ps5_svc PRIVATE NXRT_SVC_NO_IPC=1
    NXRT_DISABLE_FRAME_TRACE=1 NXRT_DISABLE_TEST_TOOLING=1)
target_compile_options(nxrt_ps5_svc PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(nxrt_ps5_svc PUBLIC nxrt_ps5_kernel)
# PS5 memory handlers currently cover heap size, protection, attributes and
# queries. Unported calls keep the dispatcher's explicit not-implemented result.
