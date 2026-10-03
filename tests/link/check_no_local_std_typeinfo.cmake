# FM-145 guard: fail if any built executable carries a local (hidden) copy of a
# std:: typeinfo. Such a copy makes `catch (const std::exception &)` miss
# exceptions thrown with libc++'s typeinfo. Inputs: LIST_FILE (one executable
# path per line), NM (path to nm).
if(NOT CMAKE_HOST_APPLE)
    message("SKIPPED: the typeinfo guard reads Mach-O `nm -m` output and only runs on macOS")
    return()
endif()
if(NOT NM)
    set(NM nm)
endif()

file(STRINGS "${LIST_FILE}" exes)
list(LENGTH exes count)
if(count EQUAL 0)
    message(FATAL_ERROR "no executables listed in ${LIST_FILE}")
endif()

set(bad "")
set(missing "")
foreach(exe IN LISTS exes)
    if(NOT EXISTS "${exe}")
        list(APPEND missing "${exe}")
        continue()
    endif()
    execute_process(COMMAND "${NM}" -m "${exe}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NM} -m ${exe} failed (${rc}): ${err}")
    endif()
    # A local definition prints as "non-external (was a private external) __ZTISt...".
    string(REGEX MATCHALL "non-external[^\n]* __ZTISt[^\n]*" hits "${out}")
    if(hits)
        list(APPEND bad "${exe}")
        foreach(h IN LISTS hits)
            message("LOCAL STD TYPEINFO: ${exe}: ${h}")
        endforeach()
    endif()
endforeach()

if(missing)
    string(REPLACE ";" "\n  " missing_text "${missing}")
    message(FATAL_ERROR "executables not built (build the full tree before ctest):\n  ${missing_text}")
endif()
if(bad)
    list(LENGTH bad nbad)
    message(FATAL_ERROR "${nbad} of ${count} executables carry a local std typeinfo (FM-145); "
        "catch (const std::exception &) is unreliable in them")
endif()
message("OK: ${count} executables, none carries a local std typeinfo")
