cmake_minimum_required(VERSION 3.22)

project(sentinel_core)

add_library(sentinel_core
    Authenticator.cpp
    Authenticator.hpp
    CoinbaseStreamClient.hpp
    CoinbaseStreamClient.cpp
    CvdThresholdRule.cpp
    CvdThresholdRule.h
    DataCache.cpp
    DataCache.hpp
    LockFreeQueue.h
    MarketDataCore.cpp
    MarketDataCore.hpp
    MessageParser.hpp
    PerformanceMonitor.cpp
    PerformanceMonitor.h
    Rule.h
    RuleEngine.cpp
    RuleEngine.h
    SentinelLogging.cpp
    SentinelLogging.hpp
    StatisticsController.cpp
    StatisticsController.h
    StatisticsProcessor.cpp
    StatisticsProcessor.h
    TradeData.h
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
# jwt-cpp is fetched via FetchContent in main CMakeLists.txt

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