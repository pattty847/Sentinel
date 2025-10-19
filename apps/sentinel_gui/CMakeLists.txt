add_executable(sentinel_gui main.cpp)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_gui
    PRIVATE
        sentinel_gui_lib
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
) 