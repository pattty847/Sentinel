project(sentinel_server)

add_executable(sentinel_server
    main.cpp
    SentinelServerApp.cpp
    SentinelServerApp.hpp
)

target_link_libraries(sentinel_server
    PRIVATE
    sentinel_core
    Qt6::Core
    Qt6::Network
)

# Standardize output
set_target_properties(sentinel_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "sentinel-server"
)

