# SPDX-License-Identifier: GPL-3.0-or-later
set(input_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_input_services OBJECT
    "${input_root}/src/services/hid.cpp" "${input_root}/src/services/input_source.cpp"
    "${input_root}/src/services/power.cpp" "${input_root}/src/services/pl.cpp"
    "${input_root}/src/services/network.cpp")
target_compile_options(nxrt_ps5_input_services PRIVATE -Wall -Wextra -mcx16)
target_link_libraries(nxrt_ps5_input_services PUBLIC nxrt_ps5_kernel)
