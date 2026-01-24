set(GRID_CORE_SOURCES
    # Main window and controllers
    MainWindowGpu.cpp
    MainWindowGpu.h
    ChartModeController.cpp
    ChartModeController.h
    
    # MainWindow modular components
    mainwindow/DockFactory.cpp
    mainwindow/DockFactory.h
    mainwindow/QmlSceneController.cpp
    mainwindow/QmlSceneController.h
    mainwindow/LayoutOrchestrator.cpp
    mainwindow/LayoutOrchestrator.h
    mainwindow/MenuBuilder.cpp
    mainwindow/MenuBuilder.h
    mainwindow/ShortcutBinder.cpp
    mainwindow/ShortcutBinder.h
    
    # Grid system components
    datasources/IGridDataSource.hpp
    datasources/RemoteGridDataSource.hpp
    datasources/RemoteGridDataSource.cpp
    UnifiedGridRenderer.cpp
    UnifiedGridRenderer.h
    CoordinateSystem.cpp
    CoordinateSystem.h
    ChartMode.h
    
    # Axis models for QML integration
    models/AxisModel.hpp
    models/AxisModel.cpp
    models/TimeAxisModel.hpp
    models/TimeAxisModel.cpp
    models/PriceAxisModel.hpp
    models/PriceAxisModel.cpp
    
    # New modular render architecture (V2)
    render/GridViewState.hpp
    render/GridViewState.cpp
    render/DataProcessor.hpp
    render/DataProcessor.cpp
    render/HeatmapStreamState.hpp
    render/HeatmapStreamState.cpp
    render/ViewportAutoScrollController.hpp
    render/ViewportAutoScrollController.cpp
    render/HeatmapLabelRenderer.hpp
    render/HeatmapLabelRenderer.cpp
    render/GlyphAtlas.hpp
    render/GlyphAtlas.cpp
    render/MsdfAtlas.hpp
    render/MsdfAtlas.cpp
    render/HeatmapGlyphNode.hpp
    render/HeatmapGlyphNode.cpp
    render/MsdfGlyphNode.hpp
    render/MsdfGlyphNode.cpp
    render/LabTextItem.hpp
    render/LabTextItem.cpp
    render/HeatmapIntensityNode.hpp
    render/HeatmapIntensityNode.cpp
    widgets/LabDock.hpp
    widgets/LabDock.cpp
)

set(WIDGET_SOURCES
    "widgets/AICommentaryFeedDock.cpp"
    "widgets/CommentaryFeedDock.cpp"
    "widgets/CopenetFeedDock.cpp"
    "widgets/DockablePanel.cpp"
    "widgets/HeatmapDock.cpp"
    "widgets/LayoutManager.cpp"
    "marketdata/MarketDataCoreQt.cpp"
    "widgets/OrderBookDock.cpp"
    "widgets/SecApiClient.cpp"
    "widgets/SecFilingDock.cpp"
    "widgets/ServiceLocator.cpp"
    "widgets/StatusBar.cpp"
)

set(THEME_SOURCES
    "themes/DarkTheme.cpp"
    "themes/ThemeManager.cpp"
)

set(GUI_SOURCES ${GRID_CORE_SOURCES} ${WIDGET_SOURCES} ${THEME_SOURCES})

# Create the main library target
add_library(sentinel_gui_lib ${GUI_SOURCES})

# Create the QML module
qt6_add_qml_module(sentinel_gui_lib
    URI Sentinel.Charts
    VERSION 1.0
    QML_FILES
        qml/DepthChartView.qml
        qml/LabView.qml
        qml/CandleChartView.qml
        qml/controls/VolumeFilter.qml
        qml/controls/PriceResolutionSelector.qml
        qml/controls/TimeframeSelector.qml
        qml/controls/GridResolutionSelector.qml
        qml/controls/NavigationControls.qml
        qml/controls/LiquidityThresholdFilter.qml
)

qt_add_shaders(sentinel_gui_lib heatmap_shaders
    BATCHABLE
    GLSL
        "330,440"
    PREFIX
        "/shaders"
    BASE
        "render/shaders"
    FILES
        render/shaders/heatmap_intensity.vert
        render/shaders/heatmap_intensity.frag
        render/shaders/heatmap_glyph.vert
        render/shaders/heatmap_glyph.frag
        render/shaders/heatmap_msdf.vert
        render/shaders/heatmap_msdf.frag
)

target_include_directories(sentinel_gui_lib 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Dependencies found in root CMakeLists.txt via vcpkg

target_link_libraries(sentinel_gui_lib
    PUBLIC
        sentinel_core
        msdfgen::msdfgen
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Charts
        Qt6::Network
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
)

qt_add_resources(sentinel_gui_lib "resources"
    PREFIX
        "/resources"
    FILES
        # Add resource files here if any, e.g., icons
)

# Automatically run MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# =============================================================================
# MIGRATION SAFETY: ENHANCED COMPILER WARNINGS
# =============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sentinel_gui_lib PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-sign-conversion   # Qt/Boost has many sign conversions
        -Wno-shadow           # Qt macros cause shadow warnings
    )
endif()
