set(GRID_CORE_SOURCES
    # Main window and controllers
    MainWindowGpu.cpp
    MainWindowGpu.h
    ChartModeController.cpp
    ChartModeController.h
    PerformanceMonitor.cpp
    PerformanceMonitor.hpp
    TradeInputManager.hpp
    TradeInputManager.cpp
    config/GuiConfigStore.cpp
    config/GuiConfigStore.hpp

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
    datasources/CandleSeriesBuffer.hpp
    datasources/CandleSeriesBuffer.cpp
    UnifiedGridRenderer.cpp
    UnifiedGridRenderer.Init.cpp
    UnifiedGridRenderer.Render.cpp
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
    render/FootprintStreamState.hpp
    render/FootprintStreamState.cpp
    render/TpoStreamState.hpp
    render/TpoStreamState.cpp
    render/FootprintOverlayRenderer.hpp
    render/FootprintOverlayRenderer.cpp
    render/TpoOverlayRenderer.hpp
    render/TpoOverlayRenderer.cpp
    render/VolumeProfileState.hpp
    render/VolumeProfileState.cpp
    render/VolumeProfileRenderer.hpp
    render/VolumeProfileRenderer.cpp
    render/CandlestickOverlayItem.hpp
    render/CandlestickOverlayItem.cpp
    render/ITimeAxisMappingProvider.hpp
    render/TimeAuthority.hpp
    render/TimeAuthority.cpp
    render/ViewportAutoScrollController.hpp
    render/ViewportAutoScrollController.cpp
    render/TimeAxisMapping.hpp
    render/UgrFrameMath.hpp
    render/UgrFrameMath.cpp
    render/HeatmapLabelRenderer.hpp
    render/HeatmapLabelRenderer.cpp
    render/ChartTextPrimitives.hpp
    render/ChartTextAtlas.hpp
    render/ChartTextAtlas.cpp
    render/ChartTextLayout.hpp
    render/ChartTextLayout.cpp
    render/ChartTextDebugNode.hpp
    render/ChartTextDebugNode.cpp
    render/ChartTextNode.hpp
    render/ChartTextNode.cpp
    render/ChartTextRenderer.hpp
    render/ChartTextRenderer.cpp
    render/HeatmapOverlayRenderer.hpp
    render/HeatmapOverlayRenderer.cpp
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
    render/FootprintIntensityNode.hpp
    render/FootprintIntensityNode.cpp
    widgets/LabDock.hpp
    widgets/LabDock.cpp
    render/AlgoOverlayRenderer.hpp
    render/AlgoOverlayRenderer.cpp
    render/PnlCurveItem.hpp
    render/PnlCurveItem.cpp
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
    "widgets/ScreenerDock.cpp"
    "widgets/ServiceLocator.cpp"
    "widgets/StatusBar.cpp"
    "widgets/HeatmapSettingsDialog.cpp"
    "widgets/TopToolbar.cpp"
    "widgets/WatchlistDock.cpp"
    "widgets/StockChartDock.cpp"
    "widgets/TradeBlotterDock.cpp"
    "widgets/TradeBlotterDock.hpp"
    "widgets/PaperTradingDock.hpp"
    "widgets/PaperTradingDock.cpp"
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
    SOURCES
        render/CandlestickOverlayItem.hpp
        render/CandlestickOverlayItem.cpp
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
        qml/StockChartView.qml
)

set(SENTINEL_SVG_DIR "${CMAKE_SOURCE_DIR}/resources/svg")
set(SENTINEL_SVG_FILES
    "${SENTINEL_SVG_DIR}/browse.svg"
    "${SENTINEL_SVG_DIR}/camera.svg"
    "${SENTINEL_SVG_DIR}/candlestick_chart.svg"
    "${SENTINEL_SVG_DIR}/full_screen.svg"
    "${SENTINEL_SVG_DIR}/grid_view.svg"
    "${SENTINEL_SVG_DIR}/indicators.svg"
    "${SENTINEL_SVG_DIR}/layout.svg"
    "${SENTINEL_SVG_DIR}/line_chart.svg"
    "${SENTINEL_SVG_DIR}/record.svg"
    "${SENTINEL_SVG_DIR}/reset_settings.svg"
    "${SENTINEL_SVG_DIR}/search.svg"
    "${SENTINEL_SVG_DIR}/settings.svg"
    "${SENTINEL_SVG_DIR}/tpo_chart.svg"
    "${SENTINEL_SVG_DIR}/footprint.svg"
    "${SENTINEL_SVG_DIR}/refresh.svg"
    "${SENTINEL_SVG_DIR}/auto-refresh.svg"
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

set_source_files_properties("${SENTINEL_SVG_DIR}/browse.svg" PROPERTIES QT_RESOURCE_ALIAS "browse.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/camera.svg" PROPERTIES QT_RESOURCE_ALIAS "camera.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/candlestick_chart.svg" PROPERTIES QT_RESOURCE_ALIAS "candlestick_chart.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/full_screen.svg" PROPERTIES QT_RESOURCE_ALIAS "full_screen.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/grid_view.svg" PROPERTIES QT_RESOURCE_ALIAS "grid_view.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/indicators.svg" PROPERTIES QT_RESOURCE_ALIAS "indicators.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/layout.svg" PROPERTIES QT_RESOURCE_ALIAS "layout.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/line_chart.svg" PROPERTIES QT_RESOURCE_ALIAS "line_chart.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/record.svg" PROPERTIES QT_RESOURCE_ALIAS "record.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/reset_settings.svg" PROPERTIES QT_RESOURCE_ALIAS "reset_settings.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/search.svg" PROPERTIES QT_RESOURCE_ALIAS "search.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/settings.svg" PROPERTIES QT_RESOURCE_ALIAS "settings.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/tpo_chart.svg" PROPERTIES QT_RESOURCE_ALIAS "tpo_chart.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/footprint.svg" PROPERTIES QT_RESOURCE_ALIAS "footprint.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/refresh.svg" PROPERTIES QT_RESOURCE_ALIAS "refresh.svg")
set_source_files_properties("${SENTINEL_SVG_DIR}/auto-refresh.svg" PROPERTIES QT_RESOURCE_ALIAS "auto-refresh.svg")

qt_add_resources(sentinel_gui_lib sentinel_svg_resources
    PREFIX "/svg"
    FILES
        ${SENTINEL_SVG_FILES}
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
        render/shaders/footprint_intensity.vert
        render/shaders/footprint_intensity.frag
)

target_include_directories(sentinel_gui_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/render
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
        Qt6::WebSockets
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
