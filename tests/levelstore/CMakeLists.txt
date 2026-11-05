# Architecture C (libs/core/levelstore). test_level_recorder includes no Qt header;
# test_level_roll uses QtCore only through the capture writer's fixture config.
add_executable(test_exact_decimal test_exact_decimal.cpp)
target_include_directories(test_exact_decimal PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_exact_decimal PRIVATE GTest::gtest_main)
add_test(NAME ExactDecimalTests COMMAND test_exact_decimal)
set_tests_properties(ExactDecimalTests PROPERTIES LABELS "levelstore;fast")

add_executable(test_level_recorder test_level_recorder.cpp test_level_coverage.cpp)
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

add_executable(test_level_log test_level_log.cpp)
target_include_directories(test_level_log PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_log PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME LevelLogTests COMMAND test_level_log)
set_tests_properties(LevelLogTests PROPERTIES LABELS "levelstore" TIMEOUT 180)

add_executable(levelstore_log_replay levelstore_log_replay.cpp)
target_include_directories(levelstore_log_replay PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(levelstore_log_replay PRIVATE sentinel_core Qt6::Core)

add_executable(test_level_restart test_level_restart.cpp RestartJournal.cpp)
target_include_directories(test_level_restart PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_restart PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME LevelRestartTests COMMAND test_level_restart)
set_tests_properties(LevelRestartTests PROPERTIES LABELS "levelstore" TIMEOUT 600)
add_executable(levelstore_restart_replay levelstore_restart_replay.cpp)
target_include_directories(levelstore_restart_replay PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(levelstore_restart_replay PRIVATE sentinel_core)

add_executable(test_level_restart_sweep test_level_restart_sweep.cpp RestartJournal.cpp)
target_include_directories(test_level_restart_sweep PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_restart_sweep PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME LevelRestartSweepTests COMMAND test_level_restart_sweep)
set_tests_properties(LevelRestartSweepTests PROPERTIES LABELS "levelstore" TIMEOUT 1800)

find_package(zstd CONFIG REQUIRED)
add_executable(decimal_domain_survey decimal_domain_survey.cpp ${CMAKE_SOURCE_DIR}/libs/core/Crc32.cpp)
target_include_directories(decimal_domain_survey PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(decimal_domain_survey PRIVATE nlohmann_json::nlohmann_json
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)

add_executable(test_d2_codec test_d2_codec.cpp)
target_include_directories(test_d2_codec PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_d2_codec PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME D2CodecTests COMMAND test_d2_codec)
set_tests_properties(D2CodecTests PROPERTIES LABELS "levelstore;fast")

add_executable(test_level_halt test_level_halt.cpp RestartJournal.cpp)
target_include_directories(test_level_halt PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_level_halt PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME LevelHaltTests COMMAND test_level_halt)
set_tests_properties(LevelHaltTests PROPERTIES LABELS "levelstore" TIMEOUT 600)

add_executable(test_seal_store test_seal_store.cpp)
target_include_directories(test_seal_store PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_seal_store PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME SealStoreTests COMMAND test_seal_store)
set_tests_properties(SealStoreTests PROPERTIES LABELS "levelstore" TIMEOUT 600)

add_executable(levelstore_bench levelstore_bench.cpp)
target_include_directories(levelstore_bench PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(levelstore_bench PRIVATE nlohmann_json::nlohmann_json)
