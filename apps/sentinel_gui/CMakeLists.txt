add_executable(sentinel_gui main.cpp)

find_package(Boost REQUIRED COMPONENTS beast)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_gui
    PRIVATE
        sentinel_gui_lib
        Boost::beast
        OpenSSL::SSL
        OpenSSL::Crypto
) 