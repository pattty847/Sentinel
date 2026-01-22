add_executable(sentinel_gui main.cpp)

target_link_libraries(sentinel_gui
    PRIVATE
        sentinel_gui_lib
)

if (WIN32)
    # Automatically deploy Qt dependencies next to the executable,
    # and scan our QML sources so QtQuick.Controls and friends are included.
    add_custom_command(TARGET sentinel_gui POST_BUILD
        COMMAND "$ENV{QT_MSVC}/bin/windeployqt.exe"
                --qmldir "${CMAKE_SOURCE_DIR}/libs/gui/qml"
                "$<TARGET_FILE:sentinel_gui>"
        COMMENT "Running windeployqt for sentinel_gui"
        VERBATIM
    )
endif()