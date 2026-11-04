# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(pokopia_ps5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PKP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(PKP_INPUT "C:/Games/pokopia" CACHE PATH "Existing decrypted game tree")
set(NX_PYTHON "python3" CACHE FILEPATH "Host Python with recomp/requirements.txt installed")
set(PKP_LIFT_JOBS 2 CACHE STRING "Worker processes per lifted module")
if(NOT PS5 OR NOT PROSPERO OR NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    message(FATAL_ERROR "Use the PS5 payload SDK toolchain/prospero.cmake")
endif()

execute_process(COMMAND "${NX_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/validate_input.py" --input "${PKP_INPUT}"
                RESULT_VARIABLE validate_result)
if(NOT validate_result EQUAL 0)
    message(FATAL_ERROR "Decrypted input validation failed")
endif()
file(READ "${PKP_ROOT}/work/ps5/input-manifest.json" input_manifest)
string(JSON PKP_EXEFS GET "${input_manifest}" exefs)
include("${PKP_ROOT}/recomp/cmake/NxRecomp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/platform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kernel.cmake")

# Keep the upstream addresses until the runtime and RADV allocation domains
# have been partitioned together. These archives do not map or execute them.
nxrecomp_add_module(pkp_main INPUT "${PKP_EXEFS}/main" NAME main INDEX 0 BASE 0x7100000000
    GAME pkp SHARDS 230 JOBS ${PKP_LIFT_JOBS} SITES "${PKP_ROOT}/recomp/games/pkp/main.sites"
    OUTDIR "${CMAKE_BINARY_DIR}/lift/main")
nxrecomp_add_module(pkp_sdk INPUT "${PKP_EXEFS}/sdk" NAME sdk INDEX 1 BASE 0x7500000000
    GAME pkp SHARDS 45 JOBS ${PKP_LIFT_JOBS} OUTDIR "${CMAKE_BINARY_DIR}/lift/sdk")
nxrecomp_add_module(pkp_subsdk0 INPUT "${PKP_EXEFS}/subsdk0" NAME subsdk0 INDEX 2 BASE 0x7600000000
    GAME pkp SHARDS 4 JOBS ${PKP_LIFT_JOBS} OUTDIR "${CMAKE_BINARY_DIR}/lift/subsdk0")
nxrecomp_add_module(pkp_rtld INPUT "${PKP_EXEFS}/rtld" NAME rtld INDEX 3 BASE 0x7700000000
    GAME pkp SHARDS 1 JOBS ${PKP_LIFT_JOBS} OUTDIR "${CMAKE_BINARY_DIR}/lift/rtld")
add_custom_target(pkp_modules ALL DEPENDS pkp_main pkp_sdk pkp_subsdk0 pkp_rtld)
foreach(module pkp_main pkp_sdk pkp_subsdk0 pkp_rtld)
    target_compile_definitions(${module} PUBLIC NXRECOMP_SUBPAGE_CHECKS=1)
endforeach()
add_custom_target(pkp_native_components DEPENDS pkp_modules nxrt_ps5_platform nxrt_ps5_core nxrt_ps5_kernel)
