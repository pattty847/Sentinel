add_executable(test_roller test_roller.cpp)
target_link_libraries(test_roller PRIVATE sentinel_core capture_legacy_v2_fixture GTest::gtest_main Qt6::Core)
add_test(NAME RollerTests COMMAND test_roller)
set_tests_properties(RollerTests PROPERTIES LABELS "roller" TIMEOUT 120)

target_compile_definitions(test_roller PRIVATE ROLLER_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
    ROLLER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/btc-prefix.rawl2")
add_executable(test_roller_live_fixture test_live_fixture.cpp)
target_include_directories(test_roller_live_fixture PRIVATE ${CMAKE_SOURCE_DIR}/libs/core/servermodel)
target_link_libraries(test_roller_live_fixture PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
target_compile_definitions(test_roller_live_fixture PRIVATE ROLLER_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
    ROLLER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/btc-prefix.rawl2")
add_test(NAME RollerLiveFixtureTests COMMAND test_roller_live_fixture)

add_executable(test_shadow_roller test_shadow.cpp
    ../../apps/sentinel-server/SentinelServerApp.cpp
    ../../apps/sentinel-server/SentinelServerApp.hpp)
target_link_libraries(test_shadow_roller PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME ShadowRollerTests COMMAND test_shadow_roller)
set_tests_properties(ShadowRollerTests PROPERTIES LABELS "roller" TIMEOUT 120)
add_test(NAME DeployMarkerTests COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/deploy_marker_test.sh ${CMAKE_SOURCE_DIR})
set_tests_properties(DeployMarkerTests PROPERTIES LABELS "roller" TIMEOUT 30)

add_executable(test_roller_corrupt_blocks test_corrupt_blocks.cpp)
target_link_libraries(test_roller_corrupt_blocks PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
target_compile_definitions(test_roller_corrupt_blocks PRIVATE SENTINEL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME RollerCorruptBlockTests COMMAND test_roller_corrupt_blocks)
set_tests_properties(RollerCorruptBlockTests PROPERTIES LABELS "roller" TIMEOUT 120 RUN_SERIAL TRUE)

add_executable(test_journal_anchors test_anchors.cpp)
target_compile_definitions(test_journal_anchors PRIVATE ANCHOR_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/btc-prefix.rawl2")
target_link_libraries(test_journal_anchors PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME JournalAnchorTests COMMAND test_journal_anchors)
set_tests_properties(JournalAnchorTests PROPERTIES LABELS "roller" TIMEOUT 180)
