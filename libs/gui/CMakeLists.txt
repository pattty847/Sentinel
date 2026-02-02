set(GRID_CORE_SOURCES
    # Main window and controllers
    MainWindowGpu.cpp
    MainWindowGpu.h
    ChartModeController.cpp
    ChartModeController.h
    PerformanceMonitor.cpp
    PerformanceMonitor.hpp

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
    mainwindow/GuiApiServer.cpp
    mainwindow/GuiApiServer.h
    
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
    render/CandlestickBatched.hpp
    render/CandlestickBatched.cpp
    render/MsdfAtlas.hpp
    render/MsdfAtlas.cpp
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
    "widgets/FontSettingsDialog.cpp"
    "widgets/HeatmapDock.cpp"
    "widgets/LayoutManager.cpp"
    "marketdata/MarketDataCoreQt.cpp"
    "widgets/OrderBookDock.cpp"
    "widgets/SecApiClient.cpp"
    "widgets/SecFilingDock.cpp"
    "widgets/ServiceLocator.cpp"
    "widgets/StatusBar.cpp"
    "widgets/HeatmapSettingsDialog.cpp"
    "widgets/TopToolbar.cpp"
    "widgets/WatchlistDock.cpp"
)

set(THEME_SOURCES
    "themes/DarkTheme.cpp"
    "themes/FontManager.cpp"
    "themes/ThemeBridge.cpp"
    "themes/ThemeManager.cpp"
)

set(GUI_SOURCES ${GRID_CORE_SOURCES} ${WIDGET_SOURCES} ${THEME_SOURCES})

# Create the main library target
add_library(sentinel_gui_lib ${GUI_SOURCES})

# Create the QML module
qt6_add_qml_module(sentinel_gui_lib
    URI Sentinel.Charts
    VERSION 1.0
    NO_PLUGIN
    QML_FILES
        qml/DepthChartView.qml
        qml/LabView.qml
        qml/CandleChartView.qml
        qml/Theme.qml
        qml/controls/VolumeFilter.qml
        qml/controls/PriceResolutionSelector.qml
        qml/controls/TimeframeSelector.qml
        qml/controls/GridResolutionSelector.qml
        qml/controls/NavigationControls.qml
        qml/controls/LiquidityThresholdFilter.qml
        qml/controls/ChartToolRail.qml
)

set(SENTINEL_ICON_DIR "${CMAKE_SOURCE_DIR}/resources/icons")
set(SENTINEL_ICON_FILES
    "${SENTINEL_ICON_DIR}/icon-search.svg"
    "${SENTINEL_ICON_DIR}/icon-heatmap.svg"
    "${SENTINEL_ICON_DIR}/icon-candles.svg"
    "${SENTINEL_ICON_DIR}/icon-hybrid.svg"
    "${SENTINEL_ICON_DIR}/icon-timeframe.svg"
    "${SENTINEL_ICON_DIR}/icon-indicators.svg"
    "${SENTINEL_ICON_DIR}/icon-layout.svg"
    "${SENTINEL_ICON_DIR}/icon-settings.svg"
    "${SENTINEL_ICON_DIR}/icon-fullscreen.svg"
    "${SENTINEL_ICON_DIR}/icon-camera.svg"
    "${SENTINEL_ICON_DIR}/icon-watchlist.svg"
    "${SENTINEL_ICON_DIR}/icon-draw.svg"
)

set(SENTINEL_FONT_DIR "${CMAKE_SOURCE_DIR}/resources/fonts/RobotoMono")
set(SENTINEL_FONT_FILES
    "${SENTINEL_FONT_DIR}/RobotoMono-Regular.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-Medium.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-Bold.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-Light.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-SemiBold.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-ExtraLight.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-Thin.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-Italic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-BoldItalic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-LightItalic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-MediumItalic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-SemiBoldItalic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-ExtraLightItalic.ttf"
    "${SENTINEL_FONT_DIR}/RobotoMono-ThinItalic.ttf"
)

set_source_files_properties("${SENTINEL_ICON_DIR}/icon-search.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-search.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-heatmap.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-heatmap.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-candles.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-candles.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-hybrid.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-hybrid.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-timeframe.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-timeframe.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-indicators.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-indicators.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-layout.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-layout.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-settings.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-settings.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-fullscreen.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-fullscreen.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-camera.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-camera.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-watchlist.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-watchlist.svg")
set_source_files_properties("${SENTINEL_ICON_DIR}/icon-draw.svg" PROPERTIES QT_RESOURCE_ALIAS "icon-draw.svg")

set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Regular.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Regular.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Medium.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Medium.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Bold.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Bold.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Light.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Light.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-SemiBold.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-SemiBold.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-ExtraLight.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-ExtraLight.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Thin.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Thin.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-Italic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-Italic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-BoldItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-BoldItalic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-LightItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-LightItalic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-MediumItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-MediumItalic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-SemiBoldItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-SemiBoldItalic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-ExtraLightItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-ExtraLightItalic.ttf")
set_source_files_properties("${SENTINEL_FONT_DIR}/RobotoMono-ThinItalic.ttf" PROPERTIES QT_RESOURCE_ALIAS "fonts/RobotoMono/RobotoMono-ThinItalic.ttf")

qt_add_resources(sentinel_gui_lib sentinel_ui_resources
    PREFIX "/icons"
    FILES
        ${SENTINEL_ICON_FILES}
)

qt_add_resources(sentinel_gui_lib sentinel_ui_fonts
    PREFIX "/"
    FILES
        ${SENTINEL_FONT_FILES}
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
        Qt6::Svg
)

target_compile_definitions(sentinel_gui_lib
    PRIVATE
        SENTINEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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
