add_executable(sentinel_backtest
    main.cpp
)

target_link_libraries(sentinel_backtest
    PRIVATE
        sentinel_core
)

set_target_properties(sentinel_backtest PROPERTIES OUTPUT_NAME sentinel-backtest)
