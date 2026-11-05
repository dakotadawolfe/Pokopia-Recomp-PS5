# SPDX-License-Identifier: GPL-3.0-or-later
set(audio_root "${CMAKE_CURRENT_LIST_DIR}/../runtime")
add_library(nxrt_ps5_audio OBJECT
    "${audio_root}/src/services/audio.cpp"
    "${audio_root}/src/services/audio_out.cpp"
    "${audio_root}/src/services/audio_sink.cpp"
    "${audio_root}/src/services/audio_paced.cpp"
    "${audio_root}/src/services/audio_wav.cpp"
    "${audio_root}/src/services/audio_ps5.cpp"
    "${audio_root}/src/services/events_posix.cpp"
    "${audio_root}/src/services/audren/Integration/HostAudioDevice.cpp")
target_compile_definitions(nxrt_ps5_audio PRIVATE NXRT_AUDIO_POSIX=1)
target_compile_options(nxrt_ps5_audio PRIVATE -Wall -Wextra -mcx16)
target_link_libraries(nxrt_ps5_audio PUBLIC nxrt_ps5_kernel)
if(PS5)
    target_link_libraries(nxrt_ps5_audio PUBLIC SceAudioOut)
endif()
