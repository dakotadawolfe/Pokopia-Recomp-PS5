# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(pokopia_audio_tests LANGUAGES C CXX)
set(root "${CMAKE_CURRENT_LIST_DIR}/../../..")
include("${root}/ps5/platform.cmake")
include("${root}/ps5/opus.cmake")
add_executable(ps5_audio_tests "${root}/ps5/tests/sync_main.cpp"
    "${root}/ps5/tests/test_audio_output.cpp"
    "${root}/ps5/tests/test_opus_decoder.cpp"
    "${root}/runtime/tests/test_svc_audren_sys.cpp"
    "${root}/runtime/tests/test_svc_audren_types.cpp"
    "${root}/runtime/tests/test_svc_audren_dec.cpp")
target_include_directories(ps5_audio_tests PRIVATE "${root}/runtime/tests")
target_link_libraries(ps5_audio_tests PRIVATE nxrt_ps5_platform nxrt_ps5_opus)
if(PS5)
    include("${root}/ps5/core.cmake")
    include("${root}/ps5/kernel.cmake")
    include("${root}/ps5/svc.cmake")
    target_sources(ps5_audio_tests PRIVATE "${root}/ps5/tests/test_audio_service.cpp"
        "${root}/ps5/tests/test_transfer_memory.cpp"
        "${root}/ps5/tests/test_opus_service.cpp"
        "${root}/ps5/tests/test_stack_budget.cpp"
        "${root}/ps5/tests/test_streamplay_service.cpp"
        "${root}/ps5/tests/test_offline_guest_displays.cpp"
        "${root}/ps5/tests/test_startup_dependencies.cpp" "${root}/runtime/tests/test_nxmodule.cpp" "${root}/ps5/app/thread_exit.cpp")
    target_compile_definitions(ps5_audio_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_AUDIO=1)
    target_link_libraries(ps5_audio_tests PRIVATE nxrt_ps5_svc)
    target_link_options(ps5_audio_tests PRIVATE "LINKER:--wrap,pthread_create")
    # Allow service-only checks after a game run without reopening AudioOut.
    add_executable(ps5_streamplay_tests "${root}/ps5/tests/sync_main.cpp"
        "${root}/ps5/tests/test_streamplay_service.cpp"
        "${root}/ps5/tests/test_offline_guest_displays.cpp" "${root}/ps5/app/thread_exit.cpp")
    target_include_directories(ps5_streamplay_tests PRIVATE "${root}/runtime/tests")
    target_compile_definitions(ps5_streamplay_tests PRIVATE NXRT_TEST_PS5=1 NXRT_TEST_STREAMPLAY=1)
    target_link_libraries(ps5_streamplay_tests PRIVATE nxrt_ps5_svc)
    target_link_options(ps5_streamplay_tests PRIVATE "LINKER:--wrap,pthread_create")
else()
    target_compile_definitions(ps5_audio_tests PRIVATE NXRT_AUDIO_POSIX=1 NXRT_AUDIO_TEST_DEVICE=1)
    target_sources(ps5_audio_tests PRIVATE
        "${root}/runtime/src/services/audio_sink.cpp" "${root}/runtime/src/services/audio_ps5.cpp"
        "${root}/runtime/src/services/audio_paced.cpp" "${root}/runtime/src/services/audio_wav.cpp"
        "${root}/runtime/src/services/audren/Integration/HostAudioDevice.cpp"
        "${root}/runtime/src/kernel/time_posix.cpp")
    enable_testing()
    add_test(NAME audio COMMAND ps5_audio_tests)
    set_tests_properties(audio PROPERTIES TIMEOUT 45)
    add_executable(ps5_shader_control_tests "${root}/ps5/tests/test_shader_control.cpp" "${root}/runtime/src/gfx/nvn/nvn_shaders.cpp")
    target_include_directories(ps5_shader_control_tests PRIVATE "${root}/runtime/include" "${root}/runtime/src/gfx/nvn")
    target_compile_features(ps5_shader_control_tests PRIVATE cxx_std_20)
    target_compile_definitions(ps5_shader_control_tests PRIVATE NXRT_TARGET_PS5=1 NXRT_DISABLE_FRAME_TRACE=1)
    target_compile_options(ps5_shader_control_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(ps5_shader_control_tests PRIVATE "LINKER:--gc-sections")
    target_link_libraries(ps5_shader_control_tests PRIVATE nxrt_ps5_platform)
    add_test(NAME shader_control COMMAND ps5_shader_control_tests)
endif()
