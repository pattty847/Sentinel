cmake_minimum_required(VERSION 3.22)

project(sentinel_core)

add_library(sentinel_core
    Authenticator.cpp
    Authenticator.hpp
    CoinbaseStreamClient.hpp
    CoinbaseStreamClient.cpp
    cvdthresholdrule.cpp
    cvdthresholdrule.h
    DataCache.cpp
    DataCache.hpp
    lockfreequeue.h
    MarketDataCore.cpp
    MarketDataCore.hpp
    MessageParser.hpp
    performancemonitor.cpp
    performancemonitor.h
    rule.h
    ruleengine.cpp
    ruleengine.h
    SentinelLogging.cpp
    SentinelLogging.hpp
    statisticscontroller.cpp
    statisticscontroller.h
    statisticsprocessor.cpp
    statisticsprocessor.h
    tradedata.h
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(jwt-cpp REQUIRED)

target_include_directories(sentinel_core PUBLIC .)

target_link_libraries(sentinel_core 
    PUBLIC
        Qt6::Core
    PRIVATE
        Boost::system
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        jwt-cpp::jwt-cpp
)

# Enable C++17 standard
target_compile_features(sentinel_core PRIVATE cxx_std_17)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 