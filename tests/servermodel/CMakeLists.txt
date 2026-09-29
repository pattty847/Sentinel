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

# Inspection tool for recording v2 files (not a test).
add_executable(hmc2_dump hmc2_dump.cpp)
target_include_directories(hmc2_dump PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(hmc2_dump PRIVATE sentinel_core)

add_executable(test_recording_page test_recording_page.cpp)
add_executable(test_recording_entries test_recording_entries.cpp)
target_include_directories(test_recording_entries PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_entries PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingEntriesTests COMMAND test_recording_entries)
set_tests_properties(RecordingEntriesTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_entries)
add_executable(test_recording_gpu_parity test_recording_gpu_parity.cpp)
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
target_include_directories(test_recording_gpu_parity PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_recording_gpu_parity PRIVATE sentinel_gui_lib GTest::gtest_main Qt6::GuiPrivate)
add_test(NAME RecordingGpuParityTests COMMAND test_recording_gpu_parity)
set_tests_properties(RecordingGpuParityTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_gpu_parity)
target_include_directories(test_recording_page PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_recording_page PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingPageTests COMMAND test_recording_page)
set_tests_properties(RecordingPageTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_page)
# Restart upgrade fixture independently constructs schema-3 hour frames.
target_link_libraries(test_book_recorder PRIVATE
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)

add_executable(test_recording_live test_recording_live.cpp)
target_include_directories(test_recording_live PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_live PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingLiveTests COMMAND test_recording_live)
set_tests_properties(RecordingLiveTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_live)

# Controlled S4 cost measurement (not a timing-sensitive CTest assertion).
add_executable(recording_live_bench recording_live_bench.cpp)
target_include_directories(recording_live_bench PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(recording_live_bench PRIVATE sentinel_core Qt6::Core)

add_executable(test_recording_server_stop test_recording_server_stop.cpp)
target_include_directories(test_recording_server_stop PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_server_stop PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingServerStopTests COMMAND test_recording_server_stop)
set_tests_properties(RecordingServerStopTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_server_stop)

add_executable(test_price_ladder test_price_ladder.cpp)
target_include_directories(test_price_ladder PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_price_ladder PRIVATE GTest::gtest_main)
add_test(NAME PriceLadderTests COMMAND test_price_ladder)
