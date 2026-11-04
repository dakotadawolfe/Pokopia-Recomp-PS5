# SPDX-License-Identifier: GPL-3.0-or-later
set(kernel_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_kernel STATIC
    "${kernel_root}/src/kernel/handles.cpp"
    "${kernel_root}/src/kernel/kernel_sync.cpp"
    "${kernel_root}/src/kernel/shared_memory_posix.cpp"
    "${kernel_root}/src/kernel/thread_posix.cpp"
    "${kernel_root}/src/kernel/time_posix.cpp")
target_include_directories(nxrt_ps5_kernel PUBLIC "${kernel_root}/include")
target_compile_options(nxrt_ps5_kernel PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(nxrt_ps5_kernel PUBLIC nxrt_ps5_core)
