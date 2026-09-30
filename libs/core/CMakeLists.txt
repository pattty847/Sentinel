project(sentinel_core)

add_library(sentinel_core
    Cpp20Utils.hpp
    capture/RawCapture.cpp
    capture/CaptureVerifier.cpp
    capture/CaptureSession.cpp
    capture/CaptureApp.cpp
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
    marketdata/rest/RestResolver.hpp
    marketdata/rest/CoinbaseRestClient.hpp
    marketdata/rest/CoinbaseRestClient.cpp
    marketdata/model/LiveOrderBook.cpp
    SentinelLogging.cpp
    SentinelLogging.hpp
    SentinelLogSink.cpp
    SentinelLogSink.hpp
    marketdata/model/TradeData.h
    servermodel/ServerDataModel.hpp
    servermodel/ServerDataModel.cpp
    servermodel/TradeOverlayPublisher.cpp
    servermodel/HeatmapTwapStreamer.hpp
    servermodel/HeatmapTwapStreamer.cpp
    servermodel/SymbolHotData.hpp
    servermodel/TickBinaryLogger.hpp
    servermodel/TickBinaryLogger.cpp
    servermodel/HmcolFormat.hpp
    servermodel/HmcolFormat.cpp
    servermodel/BookRecorder.cpp
    servermodel/BookRecorder.hpp
    servermodel/RecordingLive.cpp
    servermodel/RecordingLive.hpp
    servermodel/RecordingDir.hpp
    servermodel/RecordingPage.cpp
    servermodel/RecordingPage.hpp
    servermodel/RecordingChunks.cpp
    servermodel/RecordingChunks.hpp
    servermodel/ChunkService.cpp
    servermodel/ChunkService.hpp
    heatmap/SparseColumns.cpp
    heatmap/SparseColumns.hpp
    heatmap/ChunkCodec.cpp
    heatmap/ChunkCodec.hpp
    heatmap/TimeComposer.cpp
    heatmap/TimeComposer.hpp
    heatmap/NativeRows.hpp
    heatmap/BinCell.cpp
    heatmap/BinCell.hpp
    heatmap/HeatmapResolution.hpp
    heatmap/RecordingLoader.cpp
    heatmap/RecordingLoader.hpp
    heatmap/ChunkStore.cpp
    heatmap/ChunkStore.hpp
    heatmap/ChunkTransport.hpp
    heatmap/ChunkFetcher.cpp
    heatmap/ChunkFetcher.hpp
    heatmap/LocalChunkTransport.cpp
    heatmap/LocalChunkTransport.hpp
    protocol/SentinelStreamClientTransport.cpp
    protocol/SentinelStreamClientTransport.hpp
    heatmap/HeatmapSpanPlanner.cpp
    heatmap/HeatmapSpanPlanner.hpp
    heatmap/HeatmapTiles.cpp
    heatmap/HeatmapTiles.hpp
    servermodel/Hmc2Store.cpp
    servermodel/Hmc2Store.hpp
    servermodel/HeatmapColumnStore.hpp
    servermodel/HeatmapColumnStore.cpp
    servermodel/TimeframeAggregator.hpp
    servermodel/TimeframeAggregator.cpp
    protocol/SentinelStreamServer.hpp
    protocol/SentinelStreamServer.cpp
    protocol/SentinelStreamClient.hpp
    protocol/SentinelStreamClient.cpp
    protocol/SentinelStreamClientParseHelpers.hpp
    protocol/SentinelStreamClientParseHelpers.cpp
    protocol/ChunkWire.hpp
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
    trading/IAlgo.hpp
    trading/AlgoEngine.hpp
    trading/AlgoEngine.cpp
    trading/AvendellaMM.hpp
    trading/AvendellaMM.cpp
    trading/BacktestTypes.hpp
    trading/IExecutionModel.hpp
    trading/IBacktestStrategy.hpp
    trading/MarketEventSource.hpp
    trading/MarketEventSource.cpp
    trading/TradeDrivenExecutionModel.hpp
    trading/TradeDrivenExecutionModel.cpp
    trading/SimulationBroker.hpp
    trading/SimulationBroker.cpp
    trading/ReplayEngine.hpp
    trading/ReplayEngine.cpp
    trading/AlgoBacktestAdapter.hpp
    trading/AlgoBacktestAdapter.cpp
    trading/LiveTradingSession.hpp
    trading/LiveTradingSession.cpp
)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
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
        $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
)

# Keep file:line in Qt message context for release builds too (log sink prints it).
target_compile_definitions(sentinel_core PUBLIC QT_MESSAGELOGCONTEXT)

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
