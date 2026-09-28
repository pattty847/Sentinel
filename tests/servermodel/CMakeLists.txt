find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(test_timeframe_aggregator test_timeframe_aggregator.cpp)
target_include_directories(test_timeframe_aggregator PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_timeframe_aggregator PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME TimeframeAggregatorTests COMMAND test_timeframe_aggregator)
set_tests_properties(TimeframeAggregatorTests PROPERTIES LABELS "servermodel")

add_executable(test_hmcol_format test_hmcol_format.cpp)
target_include_directories(test_hmcol_format PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_hmcol_format PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HmcolFormatTests COMMAND test_hmcol_format)
set_tests_properties(HmcolFormatTests PROPERTIES LABELS "servermodel")

add_executable(test_heatmap_column_store test_heatmap_column_store.cpp)
target_include_directories(test_heatmap_column_store PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_heatmap_column_store PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HeatmapColumnStoreTests COMMAND test_heatmap_column_store)
set_tests_properties(HeatmapColumnStoreTests PROPERTIES LABELS "servermodel")

add_executable(test_heatmap_twap_streamer test_heatmap_twap_streamer.cpp)
target_include_directories(test_heatmap_twap_streamer PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_heatmap_twap_streamer PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HeatmapTwapStreamerTests COMMAND test_heatmap_twap_streamer)
set_tests_properties(HeatmapTwapStreamerTests PROPERTIES LABELS "servermodel")

add_executable(test_heatmap_history_fixture test_heatmap_history_fixture.cpp)
target_include_directories(test_heatmap_history_fixture PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_heatmap_history_fixture PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HeatmapHistoryFixtureTests COMMAND test_heatmap_history_fixture)
set_tests_properties(HeatmapHistoryFixtureTests PROPERTIES LABELS "servermodel")

# Dev tool: seeds the same fixture into a data dir for visual scrollback checks.
add_executable(hmcol_seed hmcol_seed.cpp)
target_include_directories(hmcol_seed PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(hmcol_seed PRIVATE sentinel_core)

add_executable(test_session_manager test_session_manager.cpp)
target_include_directories(test_session_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)
target_link_libraries(test_session_manager PRIVATE
    GTest::gtest_main
)
add_test(NAME SessionManagerTests COMMAND test_session_manager)
set_tests_properties(SessionManagerTests PROPERTIES LABELS "servermodel")

add_custom_target(servermodel_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L servermodel
    DEPENDS test_timeframe_aggregator test_hmcol_format test_heatmap_column_store test_heatmap_twap_streamer test_heatmap_history_fixture test_session_manager
    COMMENT "Running servermodel persistence tests"
)

message(STATUS "Servermodel tests configured (9 test suites)")

add_executable(test_recording_codec test_recording_codec.cpp)
target_include_directories(test_recording_codec PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_codec PRIVATE GTest::gtest_main)
add_test(NAME RecordingCodecTests COMMAND test_recording_codec)

foreach(name IN ITEMS book_recorder hmc2_store)
    add_executable(test_${name} test_${name}.cpp)
    target_include_directories(test_${name} PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
    target_link_libraries(test_${name} PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
    if(name STREQUAL "book_recorder")
        set(suite BookRecorderTests)
    else()
        set(suite Hmc2StoreTests)
    endif()
    add_test(NAME ${suite} COMMAND test_${name})
    set_tests_properties(${suite} PROPERTIES LABELS "servermodel")
    add_dependencies(servermodel_tests test_${name})
endforeach()
add_dependencies(servermodel_tests test_recording_codec)
set_tests_properties(RecordingCodecTests PROPERTIES LABELS "servermodel")

# Corruption fixtures construct/recompress raw HMC2 frames independently.
find_package(zstd CONFIG REQUIRED)
target_link_libraries(test_hmc2_store PRIVATE
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
