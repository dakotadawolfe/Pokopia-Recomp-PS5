# SPDX-License-Identifier: GPL-3.0-or-later
set(svc_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_svc STATIC
    "${svc_root}/src/kernel/svc.cpp" "${svc_root}/src/kernel/svc_thread.cpp")
target_compile_definitions(nxrt_ps5_svc PRIVATE NXRT_SVC_THREAD_GROUP_ONLY=1
    NXRT_DISABLE_FRAME_TRACE=1 NXRT_DISABLE_TEST_TOOLING=1)
target_compile_options(nxrt_ps5_svc PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(nxrt_ps5_svc PUBLIC nxrt_ps5_kernel)
# Memory, process-info and IPC groups are not registered until their backends
# are ported. Calls to missing handlers retain the dispatcher's error report.
