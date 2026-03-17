# Trading core tests
add_executable(test_trading_core
    test_trading_core.cpp
)

# Algo + limit fill tests
add_executable(test_algo_trading
    test_algo_trading.cpp
)

add_executable(test_backtest_core
    test_backtest_core.cpp
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

target_include_directories(test_algo_trading PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)

target_link_libraries(test_algo_trading PRIVATE
    sentinel_core
    GTest::gtest_main
)

add_test(NAME AlgoTradingTests COMMAND test_algo_trading)
set_tests_properties(AlgoTradingTests PROPERTIES LABELS "trading")

target_include_directories(test_backtest_core PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)

target_link_libraries(test_backtest_core PRIVATE
    sentinel_core
    GTest::gtest_main
)

add_test(NAME BacktestCoreTests COMMAND test_backtest_core)
set_tests_properties(BacktestCoreTests PROPERTIES LABELS "trading")
