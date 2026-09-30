#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <thread>
#include <atomic>
#include <optional>
#include <deque>
#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QVector>
#include "SentinelStreamProtocol.hpp"
#include "RecordingHistoryWire.hpp"
#include "ChunkWire.hpp"
#include <boost/asio/thread_pool.hpp>
#include <map>
#include <mutex>
#include <tuple>
#include "HeatmapSlice.hpp"
#include "FootprintSlice.hpp"
#include "TpoSlice.hpp"
#include "VolumeProfileSlice.hpp"
#include "../marketdata/model/TradeData.h"
#include "../config/ConfigTypes.hpp"
#include "../trading/TradingTypes.hpp"
#include "../trading/IAlgo.hpp"
#include "../trading/AlgoEngine.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class SentinelStreamClient : public QObject {
    friend struct TradeOverlayWireTest;
    friend struct CandleDataSourceTest;
    friend struct SentinelStreamClientWriteTest;
    friend struct HeatmapChunkWireTest;
    Q_OBJECT
public:
    struct HeatmapHistoryColumn {
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        QByteArray intensity;
        QByteArray liquidity;
        double liquidityScale = 1.0;
        QByteArray validity;
        uint64_t observedMs = 0;
        uint32_t flags = 0;
    };
    struct RecordingHistoryPage {
        QString symbol, requestId, status, layer, valueEncoding, message;
        int64_t timeframeMs = 0, requestEndMs = 0;
        int64_t scannedStartMs = 0, scannedEndMs = 0, nextEndMs = 0;
        int64_t oldestAvailableMs = 0, latestAvailableMs = 0;
        uint64_t bandGeneration = 0;
        bool exhausted = false;
        double bandLo = 0, bandTick = 0, sizeFloor = 0, codesPerOctave = 0;
        int bandRows = 0;
        QVector<HeatmapHistoryColumn> columns;
    };
    struct CandleBar {
        int64_t timeStartMs = 0;
        int64_t timeEndMs = 0;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        double volume = 0.0;
        bool isClosed = false;
    };

    // Decoded chunk replies are immutable and shared, never copied per receiver.
    using HeatmapChunkPtr = std::shared_ptr<const heatmap::ChunkFrame>;
    struct HeatmapChunkError {
        quint64 requestId = 0;     // 0 when a malformed frame carried no readable id
        heatmap::ChunkKey key;     // as echoed by the server; empty for local errors
        // Server: invalid_request | unavailable | busy | build_failed.
        // Local:  malformed (hostile/corrupt frame) | superseded (an older open
        // revision arrived after a newer one) | client_overloaded (decode backlog).
        QString code, message;
    };
    // Bounded binary decode backlog; frames beyond it are refused, not queued.
    static constexpr size_t kMaxDecodeBacklogBytes = 64U * 1024U * 1024U;

    explicit SentinelStreamClient(const std::string& host, const std::string& port,
                                  const std::string& caFile = "", QObject* parent = nullptr);
    ~SentinelStreamClient();

    void connectToServer();
    void disconnectFromServer();
    
    void subscribe(const std::string& symbol);
    void unsubscribe(const std::string& symbol);
    void requestHeatmapHistory(const std::string& symbol,
                               int64_t timeframeMs,
                               int64_t endTimeMs,
                               int count);
    void registerRecordingView(const recording::LiveView& view);
    void requestRecordingHeatmapHistory(const protocol::recordingwire::Request& request);
    static std::optional<RecordingHistoryPage> parseRecordingHistoryChunk(const nlohmann::json& msg);
    // One heatmap_chunk_request. Each start yields exactly one heatmapChunkReceived
    // (Chunk or NotModified) or heatmapChunkFailed carrying the returned id.
    // haveHash is empty or parallel to starts (0 = not held). No client-side
    // budget here: the S5 controller paces requests; the server refuses with busy.
    quint64 requestHeatmapChunks(const std::string& symbol, const std::string& source, int64_t levelMs,
                                 const std::vector<int64_t>& starts, const std::vector<uint64_t>& haveHash = {});
    void requestFootprintHistory(const std::string& symbol,
                                 int64_t timeframeMs,
                                 int64_t endTimeMs,
                                 int count);
    // requestId (required, 1..64 chars) is echoed on the reply chunk and on errors.
    // Only the most recently requested id is accepted: a chunk with any other id
    // (or none) is dropped before a single slice is emitted.
    void requestTpoHistory(const std::string& symbol,
                           int64_t timeframeMs,
                           int sessionType,
                           int64_t endTimeMs,
                           int count,
                           const std::string& requestId);
    // Stops an abandoned or superseded page on the server; its reply, if any, is dropped.
    void cancelTpoHistory(const std::string& symbol, const std::string& requestId);
    void requestCandleHistory(const std::string& symbol,
                              int64_t timeframeSec,
                              int64_t endTimeSec,
                              int limit);
    // Local delivery tag, captured on the network thread before queuing a bar.
    void setCandleDeliveryGeneration(quint64 generation) {
        m_candleDeliveryGeneration.store(generation, std::memory_order_release);
    }
    void requestScreenerData(const std::string& asset,
                             int limit = 50,
                             double minVolume = 0.0);
    void sendTradeCommand(const trading::TradeCommand& command);
    void sendAlgoCommand(const std::string& algoId, const std::string& action, const std::string& symbol, const trading::AlgoParams& params);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& error);
    void serverConfigReceived(const ServerConfig& config);
    
    void tradeReceived(const Trade& trade);
    // This signal is strictly for internal use by DataSource which converts prices -> indices
    void l2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates);
    
    void liveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void snapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);
    // Other signals as needed for aggregated slices
    void heatmapSliceReceived(const HeatmapSlice& slice);
    void footprintSliceReceived(const FootprintSlice& slice);
    void tpoSliceReceived(const TpoSlice& slice);
    // After the in-flight tpo_history_chunk's slices were emitted (requestId always set).
    void tpoHistoryChunkReceived(const QString& symbol, const QString& requestId, qint64 timeframeMs,
                                 int sessionType, qint64 lastEndMs, int columns);
    // A trade_overlay error that carries a TPO history request_id.
    void tpoHistoryFailed(const QString& symbol, const QString& requestId, const QString& message);
    void volumeProfileSliceReceived(const VolumeProfileSlice& slice);
    void heatmapHistoryReceived(const QString& symbol,
                                int64_t timeframeMs,
                                int gridWidth,
                                int gridHeight,
                                int64_t requestEndMs,
                                int64_t oldestAvailableMs,
                                const QVector<HeatmapHistoryColumn>& columns);
    // Emitted from the chunk decode thread; connect with a queued connection.
    // chunk->kind is Chunk or NotModified.
    void heatmapChunkReceived(quint64 requestId, SentinelStreamClient::HeatmapChunkPtr chunk);
    void heatmapChunkFailed(const SentinelStreamClient::HeatmapChunkError& error);
    // Sent on subscribe and whenever it changes. Refused on a wire-version mismatch.
    void heatmapAvailabilityReceived(const protocol::chunkwire::Availability& availability);
    void recordingViewError(const QString& symbol, uint64_t generation, const QString& code,
                            const QString& message, int retryMs);
    void recordingHeatmapLiveReceived(const RecordingHistoryPage& page);
    void recordingHeatmapHistoryReceived(const RecordingHistoryPage& page);
    void recordingHeatmapHistoryError(const QString& symbol, const QString& requestId,
                                      uint64_t bandGeneration, const QString& message);
    void candleHistoryReceived(const QString& symbol,
                               int64_t timeframeSec,
                               int64_t startTimeSec,
                               int64_t endTimeSec,
                               const QVector<CandleBar>& candles);
    void candleHistoryFailed(const QString& symbol);
    void candleBarUpdateReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t bucketStartMs,
                                 int64_t seq,
                                 const CandleBar& candle, quint64 deliveryGeneration);
    void candleBarClosedReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t bucketStartMs,
                                 int64_t seq,
                                 const CandleBar& candle, quint64 deliveryGeneration);
    // Emitted when the server returns a screener_update in response to screener_request.
    // rows is the raw JSON array as a QByteArray (UTF-8); asset is "crypto" or "stock".
    void screenerUpdateReceived(const QString& asset, int rowCount, const QByteArray& rowsJson);
    void orderUpdated(const trading::OrderUpdate& update);
    void positionUpdated(const trading::PositionUpdate& update);
    void riskOrderUpdated(const trading::RiskOrderUpdate& update);
    void algoOrderEventReceived(const trading::AlgoOrderEvent& event);
    void pnlSnapshotReceived(const trading::PnlSnapshot& snapshot);
    void coinbaseLatencyReceived(int milliseconds);

private:
    void run();
    void onResolve(boost::beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(boost::beast::error_code ec, tcp::endpoint ep);
    void onSslHandshake(boost::beast::error_code ec);
    void onHandshake(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytes_transferred);
    
    void handleMessage(const std::string& msg);
    void handleBinaryMessage(std::shared_ptr<std::vector<uint8_t>> frame);
    void decodeBinaryMessage(const std::vector<uint8_t>& frame);
    bool acceptChunkOrder(const heatmap::ChunkFrame& frame);
    void handleServerConfigMessage(const nlohmann::json& msg);
    void handleSnapshotMessage(const nlohmann::json& msg);
    void handleL2UpdateMessage(const nlohmann::json& msg);
    void handleTradeMessage(const nlohmann::json& msg);
    void handleHeatmapSliceMessage(const nlohmann::json& msg);
    void handleHeatmapHistoryChunkMessage(const nlohmann::json& msg);
    void handleCandleHistoryChunkMessage(const nlohmann::json& msg);
    void handleCandleBarMessage(protocol::MessageType type, const nlohmann::json& msg);
    void handleFootprintConfigMessage(const nlohmann::json& msg);
    void handleFootprintSliceMessage(const nlohmann::json& msg);
    void handleFootprintHistoryChunkMessage(const nlohmann::json& msg);
    void handleTpoSliceMessage(const nlohmann::json& msg);
    void handleTpoHistoryChunkMessage(const nlohmann::json& msg);
    void handleOrderUpdateMessage(const nlohmann::json& msg);
    void handlePositionUpdateMessage(const nlohmann::json& msg);
    void handleRiskOrderUpdateMessage(const nlohmann::json& msg);
    void handleScreenerUpdateMessage(const nlohmann::json& msg);
    void handleVolumeProfileSliceMessage(const nlohmann::json& msg);
    void handleCoinbaseLatencyMessage(const nlohmann::json& msg);
    void handleAlgoOrderEventMessage(const nlohmann::json& msg);
    void handlePnlSnapshotMessage(const nlohmann::json& msg);

    std::string m_host;
    std::string m_port;
    
    net::io_context m_ioc;
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    std::unique_ptr<net::executor_work_guard<net::io_context::executor_type>> m_work;
    std::thread m_thread;
    
    ssl::context m_sslCtx{ssl::context::tlsv13_client};
    using WebSocket = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>>;
    // A canceled TLS/WebSocket session is not reusable. Recreate only after
    // disconnect has drained every callback borrowing its buffers.
    std::unique_ptr<WebSocket> m_ws;
    boost::beast::flat_buffer m_buffer;
    
    std::deque<std::string> m_writeQueue;
    // Strand-owned; a nonempty queue does not prove that a write is idle.
    bool m_writeInFlight = false;
    std::string m_expectedTpoRequestId;  // strand-only: the one TPO history page accepted
    
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_isConnected{false};
    std::atomic<quint64> m_candleDeliveryGeneration{0};

    std::atomic<quint64> m_nextChunkRequestId{0};
    // Replies from an older connection are dropped after reconnect.
    std::atomic<quint64> m_connectionEpoch{0};
    std::atomic<size_t> m_decodeBacklogBytes{0};
    // Newest state accepted per chunk key (decode thread; cleared on connect).
    struct ChunkOrder { bool sealed = false; uint64_t revision = 0; int64_t committedThroughMs = 0; };
    std::mutex m_chunkOrderMutex;
    std::map<std::tuple<std::string, std::string, int64_t, int64_t>, ChunkOrder> m_chunkOrder;
    static constexpr size_t kMaxChunkOrderKeys = 65536;
    // One thread: decodes stay FIFO in arrival order. Declared last so it is
    // joined (in the destructor) before anything it touches is destroyed.
    std::unique_ptr<net::thread_pool> m_decodePool;
};

Q_DECLARE_METATYPE(SentinelStreamClient::HeatmapHistoryColumn)
Q_DECLARE_METATYPE(SentinelStreamClient::RecordingHistoryPage)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::HeatmapHistoryColumn>)
Q_DECLARE_METATYPE(SentinelStreamClient::CandleBar)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::CandleBar>)
Q_DECLARE_METATYPE(SentinelStreamClient::HeatmapChunkPtr)
Q_DECLARE_METATYPE(SentinelStreamClient::HeatmapChunkError)
Q_DECLARE_METATYPE(protocol::chunkwire::Availability)
