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
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/sentinel-server/$<CONFIG>"
    OUTPUT_NAME "sentinel-server"
)

if (WIN32)
    # Deploy Qt dependencies
    add_custom_command(TARGET sentinel_server POST_BUILD
        COMMAND "$ENV{QT_MSVC}/bin/windeployqt.exe"
                --no-opengl
                --no-system-d3d-compiler
                --no-translations
                "$<TARGET_FILE:sentinel_server>"
        COMMENT "Running windeployqt for sentinel-server"
        VERBATIM
    )
endif()
