# Link-level guards (FM-145).
#
# A static library compiled with -fno-rtti -fexceptions -fvisibility=hidden emits
# its own hidden copy of the typeinfo of every std exception it throws or catches
# (vcpkg skia's raw.SkRawCodec.o did, for std::exception, logic_error, length_error,
# bad_alloc, bad_array_new_length). ld64 then picks that archive member to satisfy
# our references before it reaches libc++.dylib, every handler in the image binds
# to the private copy, and `catch (const std::exception &)` misses exceptions whose
# typeinfo chain comes from libc++ (std::runtime_error, std::invalid_argument, ...).
#
# This directory must be added last: the scan below sees every executable target
# that exists when it runs.

# Runtime guard: links the same libraries as sentinel-gui (sentinel_gui_lib pulls
# msdfgen and everything msdfgen brings in) and checks base-class handlers.
add_executable(test_exception_typeinfo test_exception_typeinfo.cpp)
target_link_libraries(test_exception_typeinfo PRIVATE sentinel_gui_lib GTest::gtest_main)
add_test(NAME ExceptionTypeinfoTests COMMAND test_exception_typeinfo)
set_tests_properties(ExceptionTypeinfoTests PROPERTIES LABELS "link")

# Static guard: no built executable may carry a local definition of a std
# typeinfo (`nm -m` reports "non-external (was a private external)").
function(sentinel_collect_executables dir out)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(found "")
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        get_target_property(excluded ${t} EXCLUDE_FROM_ALL)
        if(type STREQUAL "EXECUTABLE" AND NOT excluded)
            list(APPEND found ${t})
        endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        sentinel_collect_executables("${sub}" sub_found)
        list(APPEND found ${sub_found})
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

sentinel_collect_executables("${CMAKE_SOURCE_DIR}" sentinel_all_executables)
set(sentinel_exe_files "")
foreach(t IN LISTS sentinel_all_executables)
    string(APPEND sentinel_exe_files "$<TARGET_FILE:${t}>\n")
endforeach()
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/typeinfo_guard_$<CONFIG>.txt"
    CONTENT "${sentinel_exe_files}")

add_test(NAME NoLocalStdTypeinfo
    COMMAND ${CMAKE_COMMAND}
        -DLIST_FILE=${CMAKE_CURRENT_BINARY_DIR}/typeinfo_guard_$<CONFIG>.txt
        -DNM=${CMAKE_NM}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_local_std_typeinfo.cmake)
set_tests_properties(NoLocalStdTypeinfo PROPERTIES
    LABELS "link"
    SKIP_REGULAR_EXPRESSION "SKIPPED:")
