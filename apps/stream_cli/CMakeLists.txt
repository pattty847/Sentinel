add_executable(sentinel_cli stream_main.cpp)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_cli
    PRIVATE
        sentinel_core
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
) 