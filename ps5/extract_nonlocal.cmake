# SPDX-License-Identifier: GPL-3.0-or-later
# The public SDK supplies FreeBSD's _setjmp/_longjmp in one standalone object.
# Private names prevent the native companion's compatibility exports replacing it.
execute_process(COMMAND "${AR}" p "${ARCHIVE}" _setjmp.o
    OUTPUT_FILE "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot extract the SDK's nonlocal jump object")
endif()
execute_process(COMMAND "${OBJCOPY}"
    --redefine-sym _setjmp=nxrt_ps5_setjmp --redefine-sym _longjmp=nxrt_ps5_longjmp "${OUTPUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot give the nonlocal jump implementation private symbols")
endif()
