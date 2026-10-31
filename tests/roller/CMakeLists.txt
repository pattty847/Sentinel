add_executable(test_roller test_roller.cpp)
target_link_libraries(test_roller PRIVATE sentinel_core capture_legacy_v2_fixture GTest::gtest_main Qt6::Core)
add_test(NAME RollerTests COMMAND test_roller)
set_tests_properties(RollerTests PROPERTIES LABELS "roller" TIMEOUT 120)

target_compile_definitions(test_roller PRIVATE ROLLER_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
    ROLLER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/btc-prefix.rawl2")
add_executable(test_shadow_roller test_shadow.cpp
    ../../apps/sentinel-server/SentinelServerApp.cpp
    ../../apps/sentinel-server/SentinelServerApp.hpp)
target_link_libraries(test_shadow_roller PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME ShadowRollerTests COMMAND test_shadow_roller)
set_tests_properties(ShadowRollerTests PROPERTIES LABELS "roller" TIMEOUT 300)
add_test(NAME DeployMarkerTests COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/deploy_marker_test.sh ${CMAKE_SOURCE_DIR})
set_tests_properties(DeployMarkerTests PROPERTIES LABELS "roller" TIMEOUT 30)

add_executable(test_roller_corrupt_blocks test_corrupt_blocks.cpp)
target_link_libraries(test_roller_corrupt_blocks PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
target_compile_definitions(test_roller_corrupt_blocks PRIVATE SENTINEL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME RollerCorruptBlockTests COMMAND test_roller_corrupt_blocks)
set_tests_properties(RollerCorruptBlockTests PROPERTIES LABELS "roller" TIMEOUT 300)

add_executable(test_journal_anchors test_anchors.cpp)
target_compile_definitions(test_journal_anchors PRIVATE ANCHOR_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/btc-prefix.rawl2")
target_link_libraries(test_journal_anchors PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME JournalAnchorTests COMMAND test_journal_anchors)
set_tests_properties(JournalAnchorTests PROPERTIES LABELS "roller" TIMEOUT 180)

add_executable(test_candle_restore test_candle_restore.cpp)
target_link_libraries(test_candle_restore PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
target_compile_definitions(test_candle_restore PRIVATE ROLLER_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME CandleRestoreTests COMMAND test_candle_restore)
set_tests_properties(CandleRestoreTests PROPERTIES LABELS "roller" TIMEOUT 120)

add_executable(candle_restore_bench candle_restore_bench.cpp)
target_link_libraries(candle_restore_bench PRIVATE sentinel_core Qt6::Core)

# Startup cost of the candle rebuild in the production graph (scratch copies only; not a ctest).
add_executable(candle_startup_bench candle_startup_bench.cpp
    ../../apps/sentinel-server/SentinelServerApp.cpp
    ../../apps/sentinel-server/SentinelServerApp.hpp)
target_link_libraries(candle_startup_bench PRIVATE sentinel_core Qt6::Core Qt6::Network)

# Read-only incident replay; run explicitly with a scratch report path, never in ctest.
add_executable(one_sided_replay one_sided_replay.cpp)
target_link_libraries(one_sided_replay PRIVATE sentinel_core Qt6::Core)

# Read-only sink evidence over real journals; scratch output only, never in ctest.
# Mach thread CPU accounting: macOS only.
if(APPLE)
find_package(zstd CONFIG REQUIRED)
add_executable(recorder_sink_replay recorder_sink_replay.cpp)
target_link_libraries(recorder_sink_replay PRIVATE sentinel_core Qt6::Core
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
endif()
