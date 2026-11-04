# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)
set(eh_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_eh STATIC
    "${eh_root}/src/eh/catch_call.cpp" "${eh_root}/src/eh/unwind.cpp"
    "${eh_root}/src/eh/typeinfo.cpp" "${eh_root}/src/eh/handlers.cpp")
target_compile_options(nxrt_ps5_eh PRIVATE -Wall -Wextra -Werror -mcx16)
target_link_libraries(nxrt_ps5_eh PUBLIC nxrt_ps5_core)
if(PS5)
    find_program(NX_OBJCOPY NAMES llvm-objcopy-18 llvm-objcopy REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    set(eh_libc "$ENV{PS5_PAYLOAD_SDK}/target/lib/libc.a")
    set(eh_context "${CMAKE_CURRENT_BINARY_DIR}/nxrt_ps5_nonlocal.o")
    add_custom_command(OUTPUT "${eh_context}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DOBJCOPY=${NX_OBJCOPY}"
            "-DARCHIVE=${eh_libc}" "-DOUTPUT=${eh_context}"
            -P "${CMAKE_CURRENT_LIST_DIR}/extract_nonlocal.cmake"
        DEPENDS "${eh_libc}" "${CMAKE_CURRENT_LIST_DIR}/extract_nonlocal.cmake"
        COMMENT "Linking the SDK nonlocal jump implementation with private symbols" VERBATIM)
    set_source_files_properties("${eh_context}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    target_sources(nxrt_ps5_eh PRIVATE "${eh_context}")
endif()
