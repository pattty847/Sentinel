# Phase 4: Core grid components (always included)
set(GRID_CORE_SOURCES
    # Main window and controllers
    MainWindowGpu.cpp
    MainWindowGpu.h
    ChartModeController.cpp
    ChartModeController.h
    StreamController.cpp
    StreamController.h
    
    # Grid system components
    GridIntegrationAdapter.cpp
    GridIntegrationAdapter.h
    UnifiedGridRenderer.cpp
    UnifiedGridRenderer.h
    LiquidityTimeSeriesEngine.cpp
    LiquidityTimeSeriesEngine.h
    CoordinateSystem.cpp
    CoordinateSystem.h
    ChartMode.h
)

# 🎯 PHASE 5: Legacy components ANNIHILATED! Pure grid-only now!

# 🎯 PHASE 5: Pure grid-only configuration
set(GUI_SOURCES ${GRID_CORE_SOURCES})
message(STATUS "🎯 PHASE 5: Building PURE GRID-ONLY components - legacy DESTROYED!")

qt6_add_qml_module(sentinel_gui_lib
    URI Sentinel.Charts
    VERSION 1.0
    QML_FILES
        qml/DepthChartView.qml
        qml/CandleChartView.qml
    SOURCES ${GUI_SOURCES}
)

target_include_directories(sentinel_gui_lib 
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Boost REQUIRED COMPONENTS beast)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_gui_lib
    PUBLIC
        sentinel_core
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Charts
        Qt::Network
        Qt::Quick
        Qt::Qml
        Qt::QuickWidgets
        Boost::beast
        OpenSSL::SSL
        OpenSSL::Crypto
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
        -Wno-unused-parameter  # Temporary for legacy code
        -Wno-sign-conversion   # Qt/Boost has many sign conversions
        -Wno-shadow           # Qt macros cause shadow warnings
    )
endif()

# 🎯 PHASE 5: Legacy build system ANNIHILATED! 
# No more conditional builds - pure grid supremacy! 