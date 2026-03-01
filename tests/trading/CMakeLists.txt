# Trading core tests
add_executable(test_trading_core
    test_trading_core.cpp
)

target_include_directories(test_trading_core PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)

target_link_libraries(test_trading_core PRIVATE
    sentinel_core
    GTest::gtest_main
)

add_test(NAME TradingCoreTests COMMAND test_trading_core)
set_tests_properties(TradingCoreTests PROPERTIES LABELS "trading")
