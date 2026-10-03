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

add_executable(test_shadow_roller test_shadow.cpp)
target_link_libraries(test_shadow_roller PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME ShadowRollerTests COMMAND test_shadow_roller)
set_tests_properties(ShadowRollerTests PROPERTIES LABELS "roller" TIMEOUT 120)
