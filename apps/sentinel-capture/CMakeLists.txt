add_executable(sentinel_capture main.cpp)
target_link_libraries(sentinel_capture PRIVATE sentinel_core Qt6::Core)
set_target_properties(sentinel_capture PROPERTIES OUTPUT_NAME "sentinel-capture"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/sentinel-capture$<$<BOOL:${CMAKE_CONFIGURATION_TYPES}>:/$<CONFIG>>")
