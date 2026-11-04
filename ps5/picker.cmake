# SPDX-License-Identifier: GPL-3.0-or-later
add_library(nxrt_ps5_picker STATIC
    "${CMAKE_CURRENT_LIST_DIR}/app/folder_picker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/app/folder_picker_paths.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/app/folder_picker_pad.cpp")
target_include_directories(nxrt_ps5_picker PUBLIC "${CMAKE_CURRENT_LIST_DIR}/app")
target_compile_features(nxrt_ps5_picker PUBLIC cxx_std_20)
target_compile_options(nxrt_ps5_picker PRIVATE -Wall -Wextra -Werror)
if(PS5)
    target_compile_definitions(nxrt_ps5_picker PUBLIC NXRT_TARGET_PS5=1)
    target_link_libraries(nxrt_ps5_picker PUBLIC ScePad SceUserService)
endif()
