project(sentinel_core)

add_library(sentinel_core
    Cpp20Utils.hpp
    LiquidityTimeSeriesEngine.cpp
    LiquidityTimeSeriesEngine.h
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
    SentinelLogging.cpp
    SentinelLogging.hpp
    marketdata/model/TradeData.h
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
# Dependencies found in root CMakeLists.txt via vcpkg
# - Boost (with system component)
# - nlohmann_json
# - OpenSSL
# - jwt-cpp (via FetchContent)

target_include_directories(sentinel_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sentinel_core
    PUBLIC
        Qt6::Core
        Boost::headers         # header-only components
        Boost::system          # required by beast
        OpenSSL::SSL
        OpenSSL::Crypto
    PRIVATE
        nlohmann_json::nlohmann_json
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

# GPU acceleration support
if(SENTINEL_USE_GPU)
    target_compile_definitions(sentinel_core PRIVATE USE_SENTINEL_GPU)
endif()

# C++20 standard is set globally in root CMakeLists.txt

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON) 
