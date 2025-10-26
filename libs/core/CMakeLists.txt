cmake_minimum_required(VERSION 3.22)

project(sentinel_core)

add_library(sentinel_core
    Cpp20Utils.hpp
    CvdThresholdRule.cpp
    CvdThresholdRule.h
    LiquidityTimeSeriesEngine.cpp
    LiquidityTimeSeriesEngine.h
    LockFreeQueue.h
    marketdata/MarketDataCore.cpp
    marketdata/MarketDataCore.hpp
    marketdata/auth/Authenticator.cpp
    marketdata/auth/Authenticator.hpp
    marketdata/cache/DataCache.cpp
    marketdata/cache/DataCache.hpp
    marketdata/dispatch/MessageParser.hpp
    marketdata/ws/WsTransport.hpp
    marketdata/ws/BeastWsTransport.hpp
    marketdata/ws/BeastWsTransport.cpp
    PerformanceMonitor.cpp
    PerformanceMonitor.h
    Rule.h
    RuleEngine.cpp
    RuleEngine.h
    SentinelLogging.cpp
    SentinelLogging.hpp
    SentinelMonitor.cpp
    SentinelMonitor.hpp
    StatisticsController.cpp
    StatisticsController.h
    StatisticsProcessor.cpp
    StatisticsProcessor.h
    marketdata/model/TradeData.h
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
# jwt-cpp is fetched via FetchContent in main CMakeLists.txt

target_include_directories(sentinel_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

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

# Windows-specific libraries for networking
if(WIN32)
    target_link_libraries(sentinel_core PRIVATE
        ws2_32
        wsock32
        mswsock
    )
endif()

# C++20 standard is set globally in root CMakeLists.txt

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 