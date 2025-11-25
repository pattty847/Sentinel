add_executable(sentinel_cli stream_main.cpp)

target_link_libraries(sentinel_cli
    PRIVATE
        sentinel_core
) 