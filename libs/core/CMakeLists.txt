cmake_minimum_required(VERSION 3.22)

project(sentinel_core)

add_library(sentinel_core)

# Source files for this library
target_sources(sentinel_core 
    PRIVATE
        coinbasestreamclient.cpp
        # New modular architecture files (completed refactoring)  
        Authenticator.cpp
        DataCache.cpp
        MarketDataCore.cpp
        CoinbaseStreamClientFacade.cpp
        # Existing files
        ruleengine.cpp
        cvdthresholdrule.cpp
        statisticsprocessor.cpp
        statisticscontroller.cpp
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Boost REQUIRED COMPONENTS system beast container)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(jwt-cpp REQUIRED)

target_include_directories(sentinel_core PUBLIC .)

target_link_libraries(sentinel_core 
    PUBLIC
        Qt6::Core
    PRIVATE
        Boost::system
        Boost::beast
        Boost::container
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        jwt-cpp::jwt-cpp
)

# Windows-specific libraries for networking
if(WIN32)
    target_link_libraries(sentinel_core PRIVATE ws2_32 wsock32 mswsock crypt32)
endif()

# Enable C++17 standard
target_compile_features(sentinel_core PRIVATE cxx_std_17)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 