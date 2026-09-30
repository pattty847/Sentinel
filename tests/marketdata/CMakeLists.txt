
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

# Test Target: test_protocol_validation
add_executable(test_protocol_validation test_protocol_validation.cpp)
target_include_directories(test_protocol_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_protocol_validation PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_test(NAME ProtocolValidationTests COMMAND test_protocol_validation)
set_tests_properties(ProtocolValidationTests PROPERTIES LABELS "marketdata")

# Test Target: test_sentinel_stream_client_parse_helpers
add_executable(test_sentinel_stream_client_parse_helpers test_sentinel_stream_client_parse_helpers.cpp)
target_include_directories(test_sentinel_stream_client_parse_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_sentinel_stream_client_parse_helpers PRIVATE
    sentinel_core
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    Qt6::Core
)
add_test(NAME SentinelStreamClientParseHelperTests COMMAND test_sentinel_stream_client_parse_helpers)
set_tests_properties(SentinelStreamClientParseHelperTests PROPERTIES LABELS "marketdata")

add_executable(test_recording_history_wire test_recording_history_wire.cpp)
target_include_directories(test_recording_history_wire PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_history_wire PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingHistoryWireTests COMMAND test_recording_history_wire)
set_tests_properties(RecordingHistoryWireTests PROPERTIES LABELS "marketdata")

add_executable(test_coinbase_rest_deadline test_coinbase_rest_deadline.cpp)
target_include_directories(test_coinbase_rest_deadline PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_coinbase_rest_deadline PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME CoinbaseRestDeadlineTests COMMAND test_coinbase_rest_deadline)
set_tests_properties(CoinbaseRestDeadlineTests PROPERTIES LABELS "marketdata" TIMEOUT 15)

# Custom target to run all marketdata tests
add_custom_target(marketdata_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L marketdata
    DEPENDS
        test_message_dispatcher
        test_subscription_manager
        test_protocol_validation
        test_sentinel_stream_client_parse_helpers
        test_recording_history_wire
        test_coinbase_rest_deadline
    COMMENT "Running market data refactor tests"
)

message(STATUS "Marketdata tests configured (3 test suites)")

add_executable(test_sentinel_stream_client_write test_sentinel_stream_client_write.cpp)
target_link_libraries(test_sentinel_stream_client_write PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME SentinelStreamClientWriteTests COMMAND test_sentinel_stream_client_write)
set_tests_properties(SentinelStreamClientWriteTests PROPERTIES LABELS "marketdata" TIMEOUT 30)
add_dependencies(marketdata_tests test_sentinel_stream_client_write)

add_executable(test_market_data_engine_reconnect test_market_data_engine_reconnect.cpp)
target_link_libraries(test_market_data_engine_reconnect PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
target_compile_definitions(test_market_data_engine_reconnect PRIVATE SENTINEL_TEST_CA="${CMAKE_SOURCE_DIR}/resources/certs/ca-bundle.crt")
add_test(NAME MarketDataEngineReconnectTests COMMAND test_market_data_engine_reconnect)
set_tests_properties(MarketDataEngineReconnectTests PROPERTIES LABELS "marketdata" TIMEOUT 30)
add_dependencies(marketdata_tests test_market_data_engine_reconnect)

add_executable(test_market_data_engine_connect_timeout test_market_data_engine_connect_timeout.cpp)
target_link_libraries(test_market_data_engine_connect_timeout PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
target_compile_definitions(test_market_data_engine_connect_timeout PRIVATE SENTINEL_TEST_CA="${CMAKE_SOURCE_DIR}/resources/certs/ca-bundle.crt")
add_test(NAME MarketDataEngineConnectTimeoutTests COMMAND test_market_data_engine_connect_timeout)
set_tests_properties(MarketDataEngineConnectTimeoutTests PROPERTIES LABELS "marketdata" TIMEOUT 60)
add_dependencies(marketdata_tests test_market_data_engine_connect_timeout)
