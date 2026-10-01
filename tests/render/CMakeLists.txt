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

# S6b: GridViewState clamps (spec rules 1, 2, 9): wheel, axis drags and direct viewports.
add_executable(test_GridViewState test_GridViewState.cpp)
target_include_directories(test_GridViewState PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_GridViewState PRIVATE GTest::gtest sentinel_gui_lib Qt6::Core Qt6::Gui)
add_test(NAME GridViewStateTests COMMAND test_GridViewState)
set_tests_properties(GridViewStateTests PROPERTIES LABELS "render")

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

# S4: GPU price binning parity vs heatmap::binColumn and grid/threshold unit
# tests. GPU cases skip without a compute-capable QRhi;
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
# S5c: HeatmapTileNode on the SpanSet (retention, cap, capacity reports, QRhi
# loss, never-blank transitions) and the bin kernel's fill pass vs the CPU oracle.
add_executable(test_heatmap_tile_node test_heatmap_tile_node.cpp)
target_include_directories(test_heatmap_tile_node PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_tile_node PRIVATE sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::Quick)
add_test(NAME HeatmapTileNodeTests COMMAND test_heatmap_tile_node)
set_tests_properties(HeatmapTileNodeTests PROPERTIES LABELS "render;heatmap")
# S5L-c: the live edge in the node (draw clip E/L vs the composed oracle, partial
# column, rollover coverage, held/fading retention, no buffer churn, disconnect).
add_executable(test_heatmap_live_node test_heatmap_live_node.cpp)
target_include_directories(test_heatmap_live_node PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_heatmap_live_node PRIVATE sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::Quick)
add_test(NAME HeatmapLiveNodeTests COMMAND test_heatmap_live_node)
set_tests_properties(HeatmapLiveNodeTests PROPERTIES LABELS "render;heatmap")
# S5c: the lab chart on the production path over a synthetic HMC2 recording.
add_executable(test_lab_item test_lab_item.cpp)
target_include_directories(test_lab_item PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_lab_item PRIVATE sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::Quick)
add_test(NAME LabItemTests COMMAND test_lab_item)
set_tests_properties(LabItemTests PROPERTIES LABELS "render;heatmap;lab")
# S6b: the main chart (UnifiedGridRenderer) on the GPU heatmap path over a
# synthetic HMC2 recording: settles, column/candle alignment, tf switch coverage,
# QRhi recreation, TPO hosting, renderer flips, clamps, legacy palette parity and
# the Agent API input route on the real DepthChartView.qml.
find_package(Qt6 REQUIRED COMPONENTS QuickPrivate)
add_executable(test_ugr_gpu test_ugr_gpu.cpp)
target_include_directories(test_ugr_gpu PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_ugr_gpu PRIVATE sentinel_gui_lib sentinel_lab_lib GTest::gtest Qt6::GuiPrivate Qt6::QuickPrivate Qt6::Quick Qt6::Qml)
target_compile_definitions(test_ugr_gpu PRIVATE SENTINEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME UgrGpuTests COMMAND test_ugr_gpu)
set_tests_properties(UgrGpuTests PROPERTIES LABELS "render;heatmap" TIMEOUT 300)
# Review blocker 1 under Guard Malloc: the data service dies before the chart.
if(APPLE AND EXISTS /usr/lib/libgmalloc.dylib)
    add_test(NAME UgrGpuServiceTeardownGuardMalloc COMMAND test_ugr_gpu "--gtest_filter=UgrGpu.TheServiceMayDieBeforeTheChart")
    set_tests_properties(UgrGpuServiceTeardownGuardMalloc PROPERTIES LABELS "render;heatmap" TIMEOUT 300
                         ENVIRONMENT "DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib")
endif()
# Lab outputs never land inside the recording root (links, aliases, missing descendants).
add_executable(test_lab_output_guard test_lab_output_guard.cpp)
target_include_directories(test_lab_output_guard PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_lab_output_guard PRIVATE sentinel_lab_lib GTest::gtest_main Qt6::Core)
target_compile_definitions(test_lab_output_guard PRIVATE SENTINEL_LAB_EXECUTABLE="$<TARGET_FILE:sentinel_lab>")
add_dependencies(test_lab_output_guard sentinel_lab)
add_test(NAME LabOutputGuardTests COMMAND test_lab_output_guard)
set_tests_properties(LabOutputGuardTests PROPERTIES LABELS "render;lab")
set_tests_properties(HeatmapGpuBinnerTests PROPERTIES LABELS "render;heatmap")
# FM-099: a readback result freed before its frame completes is a silent heap
# write without Guard Malloc; under it a wrong teardown order crashes. Covers the
# binner-first, QRhi-first and scene-graph invalidation orders.
if(APPLE AND EXISTS /usr/lib/libgmalloc.dylib)
    add_test(NAME HeatmapGpuReadbackTeardownGuardMalloc COMMAND test_heatmap_gpu_binner
             "--gtest_filter=HeatmapGpuSelfTest.DestroyingTheBinnerWithTheReadbackInFlightCompletesItFirst:HeatmapGpuLifetime.*")
    set_tests_properties(HeatmapGpuReadbackTeardownGuardMalloc PROPERTIES LABELS "render;heatmap"
                         ENVIRONMENT "DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib")
    add_test(NAME HeatmapTileNodeTeardownGuardMalloc COMMAND test_heatmap_tile_node
             "--gtest_filter=HeatmapTileNodeScene.SceneGraphInvalidation*:HeatmapTileNodeScene.QRhiDestroyedFirst*")
    set_tests_properties(HeatmapTileNodeTeardownGuardMalloc PROPERTIES LABELS "render;heatmap"
                         ENVIRONMENT "DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib")
endif()

add_executable(test_TpoProfileModel test_TpoProfileModel.cpp)
target_include_directories(test_TpoProfileModel PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_TpoProfileModel PRIVATE sentinel_gui_lib GTest::gtest_main)
add_test(NAME TpoProfileModelTests COMMAND test_TpoProfileModel)
set_tests_properties(TpoProfileModelTests PROPERTIES LABELS "render")

# S5b CPU controller: compile only CPU sources, link QtCore (no GPU required).
add_executable(test_heatmap_source_controller
    test_heatmap_source_controller.cpp
    test_heatmap_live_client.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/HeatmapSourceController.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/HeatmapSourceController.hpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/HeatmapGpuSource.cpp
)
target_include_directories(test_heatmap_source_controller PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_heatmap_source_controller PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME HeatmapSourceControllerTests COMMAND test_heatmap_source_controller)
set_tests_properties(HeatmapSourceControllerTests PROPERTIES LABELS "render;heatmap" TIMEOUT 45)

# S6a service lifecycle, ordering, settings; CPU only, no recording/live dependency.
add_executable(test_heatmap_plumbing test_heatmap_plumbing.cpp)
target_include_directories(test_heatmap_plumbing PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_heatmap_plumbing PRIVATE sentinel_lab_lib GTest::gtest_main Qt6::Core)
add_test(NAME HeatmapPlumbingTests COMMAND test_heatmap_plumbing)
set_tests_properties(HeatmapPlumbingTests PROPERTIES LABELS "render;heatmap" TIMEOUT 30)
