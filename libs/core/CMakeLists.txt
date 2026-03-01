project(sentinel_core)

add_library(sentinel_core
    Cpp20Utils.hpp
    Version.cpp
    Version.hpp
    ConfigLoader.cpp
    ConfigLoader.hpp
    marketdata/MarketDataCoreEngine.cpp
    marketdata/MarketDataCoreEngine.hpp
    marketdata/auth/Authenticator.cpp
    marketdata/auth/Authenticator.hpp
    marketdata/dispatch/MessageParser.hpp
    marketdata/ws/WsTransport.hpp
    marketdata/ws/BeastWsTransport.hpp
    marketdata/ws/BeastWsTransport.cpp
    marketdata/rest/CoinbaseRestClient.hpp
    marketdata/rest/CoinbaseRestClient.cpp
    marketdata/model/LiveOrderBook.cpp
    SentinelLogging.cpp
    SentinelLogging.hpp
    marketdata/model/TradeData.h
    servermodel/ServerDataModel.hpp
    servermodel/ServerDataModel.cpp
    servermodel/HeatmapTwapStreamer.hpp
    servermodel/HeatmapTwapStreamer.cpp
    servermodel/SymbolHotData.hpp
    servermodel/TickBinaryLogger.hpp
    servermodel/TickBinaryLogger.cpp
    servermodel/TimeframeAggregator.hpp
    servermodel/TimeframeAggregator.cpp
    protocol/SentinelStreamServer.hpp
    protocol/SentinelStreamServer.cpp
    protocol/SentinelStreamClient.hpp
    protocol/SentinelStreamClient.cpp
    protocol/SentinelStreamClientParseHelpers.hpp
    protocol/SentinelStreamClientParseHelpers.cpp
    trading/TradingTypes.hpp
    trading/TradingTypes.cpp
    trading/OrderStore.hpp
    trading/OrderStore.cpp
    trading/PositionManager.hpp
    trading/PositionManager.cpp
    trading/PaperExecutionAdapter.hpp
    trading/PaperExecutionAdapter.cpp
    trading/TradingEngine.hpp
    trading/TradingEngine.cpp
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(yaml-cpp CONFIG REQUIRED)
# Dependencies found in root CMakeLists.txt via vcpkg
# - Boost (with system component)
# - nlohmann_json
# - OpenSSL
# - jwt-cpp (via FetchContent)
# - yaml-cpp

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
        yaml-cpp::yaml-cpp
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
