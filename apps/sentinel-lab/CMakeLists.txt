add_executable(sentinel_lab main.cpp)
target_link_libraries(sentinel_lab PRIVATE sentinel_gui_lib)
set_target_properties(sentinel_lab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/sentinel-lab$<$<BOOL:${CMAKE_CONFIGURATION_TYPES}>:/$<CONFIG>>"
    OUTPUT_NAME "sentinel-lab")
qt_add_resources(sentinel_lab lab_qml PREFIX "/lab" FILES Main.qml)
