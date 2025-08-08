add_executable(sentinel_cli stream_main.cpp)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_cli
    PRIVATE
        sentinel_core
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
) 