# Architecture C (libs/core/levelstore). test_level_recorder includes no Qt header;
# test_level_roll uses QtCore only through the capture writer's fixture config.
add_executable(test_level_recorder test_level_recorder.cpp)
target_include_directories(test_level_recorder PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_recorder PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME LevelRecorderTests COMMAND test_level_recorder)
set_tests_properties(LevelRecorderTests PROPERTIES LABELS "levelstore")

add_executable(test_level_roll test_level_roll.cpp)
target_include_directories(test_level_roll PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_roll PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME LevelRollTests COMMAND test_level_roll)
set_tests_properties(LevelRollTests PROPERTIES LABELS "levelstore")

# The GoogleTest JSON report gate that evidence drivers use (gtest_report.py), checked
# by its own unit tests. -B keeps __pycache__ out of the source tree.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME GtestReportGateTests
             COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_gtest_report.py
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(GtestReportGateTests PROPERTIES LABELS "levelstore")
endif()

# Offline evidence (real RAWL2, read-only), never part of ctest.
if(APPLE)
    add_executable(levelstore_replay levelstore_replay.cpp)
    target_include_directories(levelstore_replay PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(levelstore_replay PRIVATE sentinel_core Qt6::Core)
    add_executable(levelstore_bench levelstore_bench.cpp)
    target_include_directories(levelstore_bench PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(levelstore_bench PRIVATE sentinel_core Qt6::Core)
endif()
