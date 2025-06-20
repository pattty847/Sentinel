add_library(sentinel_gui_lib
    mainwindow.cpp
    mainwindow.h
    streamcontroller.cpp
    streamcontroller.h
    tradechartwidget.cpp
    tradechartwidget.h
)

target_include_directories(sentinel_gui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sentinel_gui_lib
    PUBLIC
        sentinel_core
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Charts
        Qt::Network
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