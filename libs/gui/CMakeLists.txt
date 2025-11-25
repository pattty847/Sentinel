set(GRID_CORE_SOURCES
    # Main window and controllers
    MainWindowGpu.cpp
    MainWindowGpu.h
    ChartModeController.cpp
    ChartModeController.h
    
    # Grid system components
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
    render/IRenderStrategy.hpp
    render/IRenderStrategy.cpp
    render/GridViewState.hpp
    render/GridViewState.cpp
    render/GridSceneNode.hpp
    render/GridSceneNode.cpp
    render/RenderDiagnostics.hpp
    render/RenderDiagnostics.cpp
    render/DataProcessor.hpp
    render/DataProcessor.cpp
    render/RenderTypes.hpp
    render/RenderConfig.hpp
    render/GridTypes.hpp
    render/strategies/HeatmapStrategy.hpp
    render/strategies/HeatmapStrategy.cpp
    render/strategies/TradeFlowStrategy.hpp
    render/strategies/TradeFlowStrategy.cpp
    render/strategies/TradeBubbleStrategy.hpp
    render/strategies/TradeBubbleStrategy.cpp
    render/strategies/CandleStrategy.hpp
    render/strategies/CandleStrategy.cpp
)

set(WIDGET_SOURCES
    "widgets/AICommentaryFeedDock.cpp"
    "widgets/CommentaryFeedDock.cpp"
    "widgets/CopenetFeedDock.cpp"
    "widgets/DockablePanel.cpp"
    "widgets/HeatmapDock.cpp"
    "widgets/LayoutManager.cpp"
    "widgets/MarketDataModel.cpp"
    "widgets/MarketDataPanel.cpp"
    "widgets/OrderBookDock.cpp"
    "widgets/SecApiClient.cpp"
    "widgets/SecFilingDock.cpp"
    "widgets/ServiceLocator.cpp"
    "widgets/StatusBar.cpp"
    "widgets/StatusDock.cpp"
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
        qml/CandleChartView.qml
)

target_include_directories(sentinel_gui_lib 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Dependencies found in root CMakeLists.txt via vcpkg

target_link_libraries(sentinel_gui_lib
    PUBLIC
        sentinel_core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Charts
        Qt6::Network
        Qt6::Quick
        Qt6::Qml
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
