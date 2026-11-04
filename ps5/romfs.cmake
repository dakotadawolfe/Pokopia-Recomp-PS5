# SPDX-License-Identifier: GPL-3.0-or-later
# The same RomFS metadata builder serves the Windows and POSIX runtimes. File
# contents remain on disk and use a bounded cache of positioned-read handles.
add_library(nxrt_ps5_romfs STATIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services/fs_romfs.cpp")
target_include_directories(nxrt_ps5_romfs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../runtime/src/services")
target_compile_features(nxrt_ps5_romfs PUBLIC cxx_std_20)
target_compile_options(nxrt_ps5_romfs PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nxrt_ps5_romfs PUBLIC nxrt_ps5_platform)
