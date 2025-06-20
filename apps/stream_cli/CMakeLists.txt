add_executable(sentinel_stream stream_main.cpp)

target_link_libraries(sentinel_stream
    PRIVATE
        sentinel_core
) 