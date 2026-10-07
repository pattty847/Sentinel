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

add_executable(test_heatmap_model test_heatmap_model.cpp)
target_include_directories(test_heatmap_model PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_model PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME HeatmapModelTests COMMAND test_heatmap_model)
set_tests_properties(HeatmapModelTests PROPERTIES LABELS "servermodel;heatmap")
add_dependencies(servermodel_tests test_heatmap_model)

add_executable(test_heatmap_tick_policy test_heatmap_tick_policy.cpp)
target_include_directories(test_heatmap_tick_policy PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_tick_policy PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME HeatmapTickPolicyTests COMMAND test_heatmap_tick_policy)
set_tests_properties(HeatmapTickPolicyTests PROPERTIES LABELS "servermodel;heatmap")
add_dependencies(servermodel_tests test_heatmap_tick_policy)

add_executable(test_live_book_tick test_live_book_tick.cpp)
target_include_directories(test_live_book_tick PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_live_book_tick PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME LiveBookTickTests COMMAND test_live_book_tick)
set_tests_properties(LiveBookTickTests PROPERTIES LABELS "servermodel;marketdata")
add_dependencies(servermodel_tests test_live_book_tick)

add_executable(test_heatmap_tiles test_heatmap_tiles.cpp)
target_include_directories(test_heatmap_tiles PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_tiles PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME HeatmapTileTests COMMAND test_heatmap_tiles)
set_tests_properties(HeatmapTileTests PROPERTIES LABELS "servermodel;heatmap")
add_dependencies(servermodel_tests test_heatmap_tiles)

add_executable(test_recording_chunks test_recording_chunks.cpp)
target_include_directories(test_recording_chunks PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_chunks PRIVATE sentinel_core GTest::gtest_main Qt6::Core
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
add_test(NAME RecordingChunkTests COMMAND test_recording_chunks)
set_tests_properties(RecordingChunkTests PROPERTIES LABELS "servermodel;heatmap")
add_dependencies(servermodel_tests test_recording_chunks)

add_executable(test_recording_page test_recording_page.cpp)
target_include_directories(test_recording_page PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_recording_page PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingPageTests COMMAND test_recording_page)
set_tests_properties(RecordingPageTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_recording_page)
# Restart upgrade fixture independently constructs schema-3 hour frames.
target_link_libraries(test_book_recorder PRIVATE
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)

# Suites that start and stop a real SentinelStreamServer carry a TIMEOUT (about 10x
# their normal runtime, at least 60 s): a broken restart must fail the run, not hang it (FM-202).
add_executable(test_recording_live test_recording_live.cpp)
target_include_directories(test_recording_live PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_live PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RecordingLiveTests COMMAND test_recording_live)
set_tests_properties(RecordingLiveTests PROPERTIES LABELS "servermodel" TIMEOUT 60)
add_dependencies(servermodel_tests test_recording_live)

# Controlled S4 cost measurement (not a timing-sensitive CTest assertion).
add_executable(recording_live_bench recording_live_bench.cpp)
target_include_directories(recording_live_bench PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(recording_live_bench PRIVATE sentinel_core Qt6::Core)

add_executable(test_recording_server_stop test_recording_server_stop.cpp)
target_include_directories(test_recording_server_stop PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_recording_server_stop PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME RecordingServerStopTests COMMAND test_recording_server_stop)
set_tests_properties(RecordingServerStopTests PROPERTIES LABELS "servermodel" TIMEOUT 120)
add_dependencies(servermodel_tests test_recording_server_stop)

add_executable(test_heatmap_chunk_wire test_heatmap_chunk_wire.cpp)
target_include_directories(test_heatmap_chunk_wire PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_chunk_wire PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME HeatmapChunkWireTests COMMAND test_heatmap_chunk_wire)
set_tests_properties(HeatmapChunkWireTests PROPERTIES LABELS "servermodel;heatmap" TIMEOUT 180)
add_dependencies(servermodel_tests test_heatmap_chunk_wire)

add_executable(test_price_ladder test_price_ladder.cpp)
target_include_directories(test_price_ladder PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_price_ladder PRIVATE GTest::gtest_main)
add_test(NAME PriceLadderTests COMMAND test_price_ladder)

# Disposable storage research harness; no application links this library.
add_library(storage_probe_encoders STATIC storage_probe/Encoders.cpp)
target_include_directories(storage_probe_encoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/storage_probe
    ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(storage_probe_encoders PUBLIC sentinel_core
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
add_executable(storage_probe storage_probe/main.cpp)
target_link_libraries(storage_probe PRIVATE storage_probe_encoders Qt6::Core)
add_executable(test_storage_probe storage_probe/test_encoders.cpp)
target_link_libraries(test_storage_probe PRIVATE storage_probe_encoders GTest::gtest_main Qt6::Core)
add_test(NAME StorageProbeTests COMMAND test_storage_probe)
set_tests_properties(StorageProbeTests PROPERTIES LABELS "servermodel")
add_dependencies(servermodel_tests test_storage_probe)

add_executable(test_storage_probe_cli storage_probe/test_cli.cpp)
target_link_libraries(test_storage_probe_cli PRIVATE GTest::gtest_main Qt6::Core nlohmann_json::nlohmann_json)
target_compile_definitions(test_storage_probe_cli PRIVATE STORAGE_PROBE_EXECUTABLE="$<TARGET_FILE:storage_probe>")
add_dependencies(test_storage_probe_cli storage_probe)
add_test(NAME StorageProbeCliTests COMMAND test_storage_probe_cli)
set_tests_properties(StorageProbeCliTests PROPERTIES LABELS "servermodel" TIMEOUT 45)
add_dependencies(servermodel_tests test_storage_probe_cli)

add_executable(test_chunk_fetcher test_chunk_fetcher.cpp)
target_include_directories(test_chunk_fetcher PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_chunk_fetcher PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME ChunkFetcherTests COMMAND test_chunk_fetcher)
set_tests_properties(ChunkFetcherTests PROPERTIES LABELS "servermodel;heatmap" TIMEOUT 30)
add_dependencies(servermodel_tests test_chunk_fetcher)

add_executable(test_heatmap_span_planner test_heatmap_span_planner.cpp)
target_link_libraries(test_heatmap_span_planner PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME HeatmapSpanPlannerTests COMMAND test_heatmap_span_planner)
set_tests_properties(HeatmapSpanPlannerTests PROPERTIES LABELS "servermodel;heatmap")
add_dependencies(servermodel_tests test_heatmap_span_planner)

# /metrics wiring of ServerDataModel over the real HTTP route; records into a temp dir.
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(test_server_metrics test_server_metrics.cpp)
target_include_directories(test_server_metrics PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_server_metrics PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
add_test(NAME ServerMetricsTests COMMAND test_server_metrics)
set_tests_properties(ServerMetricsTests PROPERTIES LABELS "servermodel" TIMEOUT 60)
add_dependencies(servermodel_tests test_server_metrics)

add_executable(test_server_feed_admission test_server_feed_admission.cpp)
target_include_directories(test_server_feed_admission PRIVATE ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_server_feed_admission PRIVATE sentinel_core GTest::gtest_main Qt6::Core Qt6::Network)
target_compile_definitions(test_server_feed_admission PRIVATE SENTINEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ServerFeedAdmissionTests COMMAND test_server_feed_admission)
set_tests_properties(ServerFeedAdmissionTests PROPERTIES LABELS "servermodel" TIMEOUT 60)
add_dependencies(servermodel_tests test_server_feed_admission)

# Exercise the real process entry point with isolated config, logs and recording storage.
add_executable(test_server_startup test_server_startup.cpp)
target_link_libraries(test_server_startup PRIVATE GTest::gtest_main Qt6::Core Qt6::Network)
target_compile_definitions(test_server_startup PRIVATE
    SENTINEL_SERVER_BINARY="$<TARGET_FILE:sentinel_server>"
    SENTINEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(test_server_startup sentinel_server)
add_test(NAME ServerStartupTests COMMAND test_server_startup)
set_tests_properties(ServerStartupTests PROPERTIES LABELS "servermodel" TIMEOUT 45)
add_dependencies(servermodel_tests test_server_startup)

add_executable(test_candle_history test_candle_history.cpp)
target_include_directories(test_candle_history PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_candle_history PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME CandleHistoryCacheTests COMMAND test_candle_history)
set_tests_properties(CandleHistoryCacheTests PROPERTIES LABELS "servermodel" TIMEOUT 15)
