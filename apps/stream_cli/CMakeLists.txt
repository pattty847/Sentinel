add_executable(sentinel_stream stream_main.cpp)

find_package(Boost REQUIRED COMPONENTS beast)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_stream
    PRIVATE
        sentinel_core
        Boost::beast
        OpenSSL::SSL
        OpenSSL::Crypto
) 