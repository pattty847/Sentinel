
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

# Test Target: test_datacache_sink_adapter
add_executable(test_datacache_sink_adapter test_datacache_sink_adapter.cpp)
target_include_directories(test_datacache_sink_adapter PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_datacache_sink_adapter PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME DataCacheSinkAdapterTests COMMAND test_datacache_sink_adapter)

# Custom target to run all marketdata tests
add_custom_target(marketdata_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "MessageDispatcher|SubscriptionManager|DataCacheSinkAdapter"
    DEPENDS
        test_message_dispatcher
        test_subscription_manager
        test_datacache_sink_adapter
    COMMENT "Running market data refactor tests"
)

message(STATUS "Marketdata tests configured (3 test suites)")
