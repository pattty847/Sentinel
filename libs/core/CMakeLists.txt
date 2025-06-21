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
find_package(Boost REQUIRED COMPONENTS beast)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(sentinel_core
    PRIVATE
        Qt6::Core
        Boost::beast
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 