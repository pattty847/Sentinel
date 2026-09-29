# Render system tests — TimeAxisMapping

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_executable(test_FrameStatsWindow test_FrameStatsWindow.cpp)
target_include_directories(test_FrameStatsWindow PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_FrameStatsWindow PRIVATE GTest::gtest_main)
add_test(NAME FrameStatsWindowTests COMMAND test_FrameStatsWindow)
set_tests_properties(FrameStatsWindowTests PROPERTIES LABELS "render")

add_executable(test_PriceCenteringPolicy test_PriceCenteringPolicy.cpp)
target_include_directories(test_PriceCenteringPolicy PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_PriceCenteringPolicy PRIVATE GTest::gtest_main)
add_test(NAME PriceCenteringPolicyTests COMMAND test_PriceCenteringPolicy)
set_tests_properties(PriceCenteringPolicyTests PROPERTIES LABELS "render")

add_executable(test_TimeAxisMapping test_TimeAxisMapping.cpp)
target_include_directories(test_TimeAxisMapping PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_TimeAxisMapping PRIVATE
    GTest::gtest_main
    Qt6::Core
    Qt6::Gui
)
add_test(NAME TimeAxisMappingTests COMMAND test_TimeAxisMapping)
set_tests_properties(TimeAxisMappingTests PROPERTIES LABELS "render")

add_executable(test_UgrFrameMath
    test_UgrFrameMath.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/UgrFrameMath.cpp
)
target_include_directories(test_UgrFrameMath PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_UgrFrameMath PRIVATE
    GTest::gtest_main
    Qt6::Core
    Qt6::Gui
)
add_test(NAME UgrFrameMathTests COMMAND test_UgrFrameMath)
set_tests_properties(UgrFrameMathTests PROPERTIES LABELS "render")

add_executable(test_HeatmapColumnWindow
    test_HeatmapColumnWindow.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/HeatmapColumnWindow.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/HeatmapStreamState.cpp
)
target_include_directories(test_HeatmapColumnWindow PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_HeatmapColumnWindow PRIVATE
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HeatmapColumnWindowTests COMMAND test_HeatmapColumnWindow)
set_tests_properties(HeatmapColumnWindowTests PROPERTIES LABELS "render")

add_executable(test_CandleBackfillState test_CandleBackfillState.cpp)
target_include_directories(test_CandleBackfillState PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_CandleBackfillState PRIVATE GTest::gtest_main Qt6::Core)
add_test(NAME CandleBackfillStateTests COMMAND test_CandleBackfillState)
set_tests_properties(CandleBackfillStateTests PROPERTIES LABELS "render")

add_executable(test_CandleDataSource test_CandleDataSource.cpp)
target_include_directories(test_CandleDataSource PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_CandleDataSource PRIVATE GTest::gtest_main sentinel_gui_lib Qt6::Core)
add_test(NAME CandleDataSourceTests COMMAND test_CandleDataSource)
set_tests_properties(CandleDataSourceTests PROPERTIES LABELS "render")

add_executable(test_CandleSeriesBuffer test_CandleSeriesBuffer.cpp)
target_include_directories(test_CandleSeriesBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_CandleSeriesBuffer PRIVATE
    GTest::gtest_main
    sentinel_gui_lib
    Qt6::Core
)
add_test(NAME CandleSeriesBufferTests COMMAND test_CandleSeriesBuffer)
set_tests_properties(CandleSeriesBufferTests PROPERTIES LABELS "render")

add_executable(test_HeatmapRowGrouping test_HeatmapRowGrouping.cpp)
target_include_directories(test_HeatmapRowGrouping PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_HeatmapRowGrouping PRIVATE
    GTest::gtest_main
)
add_test(NAME HeatmapRowGroupingTests COMMAND test_HeatmapRowGrouping)
set_tests_properties(HeatmapRowGroupingTests PROPERTIES LABELS "render")

add_executable(test_AxisLayout test_AxisLayout.cpp)
target_include_directories(test_AxisLayout PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_AxisLayout PRIVATE
    GTest::gtest_main
    sentinel_gui_lib
    Qt6::Core
    Qt6::Gui
)
target_compile_definitions(test_AxisLayout PRIVATE
    SENTINEL_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME AxisLayoutTests COMMAND test_AxisLayout)
set_tests_properties(AxisLayoutTests PROPERTIES LABELS "render")

add_executable(test_PriceAxisModel test_PriceAxisModel.cpp)
target_include_directories(test_PriceAxisModel PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_PriceAxisModel PRIVATE GTest::gtest_main sentinel_gui_lib Qt6::Core)
add_test(NAME PriceAxisModelTests COMMAND test_PriceAxisModel)
set_tests_properties(PriceAxisModelTests PROPERTIES LABELS "render")

add_executable(test_RecordingBandPolicy test_RecordingBandPolicy.cpp)
target_include_directories(test_RecordingBandPolicy PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_RecordingBandPolicy PRIVATE GTest::gtest_main)
add_test(NAME RecordingBandPolicyTests COMMAND test_RecordingBandPolicy)
set_tests_properties(RecordingBandPolicyTests PROPERTIES LABELS "render")

add_executable(test_RecordingDataProcessor test_RecordingDataProcessor.cpp)
target_include_directories(test_RecordingDataProcessor PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_RecordingDataProcessor PRIVATE GTest::gtest_main sentinel_gui_lib Qt6::Core)
add_test(NAME RecordingDataProcessorTests COMMAND test_RecordingDataProcessor)
set_tests_properties(RecordingDataProcessorTests PROPERTIES LABELS "render")

add_executable(test_TradeOverlay test_TradeOverlay.cpp)
target_include_directories(test_TradeOverlay PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_TradeOverlay PRIVATE sentinel_gui_lib GTest::gtest_main)
add_test(NAME TradeOverlayTests COMMAND test_TradeOverlay)
set_tests_properties(TradeOverlayTests PROPERTIES LABELS "render;servermodel")

# S4 spike guard: compute recorded in QSGRenderNode::prepare() inside a real
# scene graph (headless via QQuickRenderControl). Skips without a Metal device.
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate Quick)
add_executable(test_qsg_compute_spike test_qsg_compute_spike.cpp)
target_include_directories(test_qsg_compute_spike PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_qsg_compute_spike PRIVATE sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::Quick)
qt_add_shaders(test_qsg_compute_spike spike_shaders
    GLSL "440"
    PREFIX "/spikeshaders"
    BASE "shaders"
    FILES
        shaders/spike_fill.comp
        shaders/spike_draw.vert
        shaders/spike_draw.frag
)
add_test(NAME QsgComputeSpikeTests COMMAND test_qsg_compute_spike)
set_tests_properties(QsgComputeSpikeTests PROPERTIES LABELS "render")

# S4: GPU price binning parity vs heatmap::binColumn, grid/threshold unit tests
# and the HeatmapRenderNode scene test. GPU cases skip without a Metal device;
# SENTINEL_HEATMAP_REAL_PARITY=1 adds the real-recording comparison.
add_executable(test_heatmap_gpu_binner test_heatmap_gpu_binner.cpp)
target_include_directories(test_heatmap_gpu_binner PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_gpu_binner PRIVATE sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::Quick)
# Test-only kernel candidate that lets fast math fold float-float error terms:
# the runtime precision self-test must reject it.
qt_add_shaders(test_heatmap_gpu_binner test_unguarded_kernel
    GLSL "440"
    HLSL 50
    MSL 12
    PREFIX "/testshaders"
    BASE "${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/shaders"
    DEFINES HEATMAP_UNGUARDED
    FILES "${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/shaders/heatmap_bin.comp"
    OUTPUTS heatmap_bin_unguarded.comp.qsb
)
add_test(NAME HeatmapGpuBinnerTests COMMAND test_heatmap_gpu_binner)
set_tests_properties(HeatmapGpuBinnerTests PROPERTIES LABELS "render;heatmap")
