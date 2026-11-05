# SPDX-License-Identifier: GPL-3.0-or-later
add_library(nxrt_ps5_volume STATIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services/fs_volume.cpp")
target_compile_options(nxrt_ps5_volume PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nxrt_ps5_volume PUBLIC nxrt_ps5_platform)
