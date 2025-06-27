qt6_add_qml_module(sentinel_gui_lib
    URI Sentinel.Charts
    VERSION 1.0
    QML_FILES
        qml/DepthChartView.qml
    SOURCES
        # 🚀 NEW GPU-FOCUSED COMPONENTS
        mainwindow_gpu.cpp
        mainwindow_gpu.h
        gpuchartwidget.cpp
        gpuchartwidget.h
        # 🔥 PHASE 2: HEATMAP INSTANCED QUADS
        heatmapinstanced.cpp
        heatmapinstanced.h
        # 🚀 PHASE 3: LOCK-FREE DATA PIPELINE
        gpudataadapter.cpp
        gpudataadapter.h
        # ✅ EXISTING DATA PIPELINE (PROVEN)
        streamcontroller.cpp
        streamcontroller.h
)

target_include_directories(sentinel_gui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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