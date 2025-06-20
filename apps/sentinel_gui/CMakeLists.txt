add_executable(sentinel main.cpp)

target_link_libraries(sentinel
    PRIVATE
        sentinel_gui_lib
) 