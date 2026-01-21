
# Find Qt components needed for tests
find_package(Qt6 REQUIRED COMPONENTS Core Test)

# Test Target: test_message_dispatcher
add_executable(test_message_dispatcher test_message_dispatcher.cpp)
target_include_directories(test_message_dispatcher PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_message_dispatcher PRIVATE
    sentinel_core
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    Qt6::Core
)
add_test(NAME MessageDispatcherTests COMMAND test_message_dispatcher)
set_tests_properties(MessageDispatcherTests PROPERTIES LABELS "marketdata")

# Test Target: test_subscription_manager
add_executable(test_subscription_manager test_subscription_manager.cpp)
target_include_directories(test_subscription_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_subscription_manager PRIVATE
    sentinel_core
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    Qt6::Core
)
add_test(NAME SubscriptionManagerTests COMMAND test_subscription_manager)
set_tests_properties(SubscriptionManagerTests PROPERTIES LABELS "marketdata")

# Custom target to run all marketdata tests
add_custom_target(marketdata_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L marketdata
    DEPENDS
        test_message_dispatcher
        test_subscription_manager
    COMMENT "Running market data refactor tests"
)

message(STATUS "Marketdata tests configured (2 test suites)")
