add_library(sentinel_core
    coinbasestreamclient.cpp
    coinbasestreamclient.h
    cvdthresholdrule.cpp
    cvdthresholdrule.h
    rule.h
    ruleengine.cpp
    ruleengine.h
    statisticscontroller.cpp
    statisticscontroller.h
    statisticsprocessor.cpp
    statisticsprocessor.h
    tradedata.h
)

target_include_directories(sentinel_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)

target_link_libraries(sentinel_core
    PRIVATE
        Qt6::Core
        Boost::program_options
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        ZLIB::ZLIB
        nlohmann_json::nlohmann_json
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 