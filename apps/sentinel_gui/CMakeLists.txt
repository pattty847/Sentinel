add_executable(sentinel main.cpp)

find_package(Boost REQUIRED COMPONENTS beast)
find_package(OpenSSL REQUIRED)

target_include_directories(sentinel PRIVATE ${CMAKE_BINARY_DIR})  # For config.h

target_link_libraries(sentinel
    PRIVATE
        sentinel_gui_lib
        Boost::beast
        OpenSSL::SSL
        OpenSSL::Crypto
) 