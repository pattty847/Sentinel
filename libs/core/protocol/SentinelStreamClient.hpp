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
#include <deque>
#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QVector>
#include "SentinelStreamProtocol.hpp"
#include "HeatmapSlice.hpp"
#include "FootprintSlice.hpp"
#include "TpoSlice.hpp"
#include "../marketdata/model/TradeData.h"
#include "../config/ConfigTypes.hpp"
#include "../trading/TradingTypes.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class SentinelStreamClient : public QObject {
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
    void requestFootprintHistory(const std::string& symbol,
                                 int64_t timeframeMs,
                                 int64_t endTimeMs,
                                 int count);
    void requestTpoHistory(const std::string& symbol,
                           int64_t timeframeMs,
                           int64_t endTimeMs,
                           int count);
    void requestCandleHistory(const std::string& symbol,
                              int64_t timeframeSec,
                              int64_t endTimeSec,
                              int limit);
    void requestScreenerData(const std::string& asset,
                             int limit = 50,
                             double minVolume = 0.0);
    void sendTradeCommand(const trading::TradeCommand& command);

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
    void heatmapSliceReceived(const HeatmapSlice& slice);
    void footprintSliceReceived(const FootprintSlice& slice);
    void tpoSliceReceived(const TpoSlice& slice);
    void heatmapHistoryReceived(const QString& symbol,
                                int64_t timeframeMs,
                                int gridWidth,
                                int gridHeight,
                                const QVector<HeatmapHistoryColumn>& columns);
    void candleHistoryReceived(const QString& symbol,
                               int64_t timeframeSec,
                               int64_t startTimeSec,
                               int64_t endTimeSec,
                               const QVector<CandleBar>& candles);
    void candleBarUpdateReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t bucketStartMs,
                                 int64_t seq,
                                 const CandleBar& candle);
    void candleBarClosedReceived(const QString& symbol,
                                 int64_t timeframeSec,
                                 int64_t bucketStartMs,
                                 int64_t seq,
                                 const CandleBar& candle);
    // Emitted when the server returns a screener_update in response to screener_request.
    // rows is the raw JSON array as a QByteArray (UTF-8); asset is "crypto" or "stock".
    void screenerUpdateReceived(const QString& asset, int rowCount, const QByteArray& rowsJson);
    void orderUpdated(const trading::OrderUpdate& update);
    void positionUpdated(const trading::PositionUpdate& update);

private:
    void run();
    void onResolve(boost::beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(boost::beast::error_code ec, tcp::endpoint ep);
    void onSslHandshake(boost::beast::error_code ec);   // NEW: between TCP connect and WS upgrade
    void onHandshake(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytes_transferred);

    void handleMessage(const std::string& msg);
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
    void handleScreenerUpdateMessage(const nlohmann::json& msg);
    void handleOrderUpdateMessage(const nlohmann::json& msg);
    void handlePositionUpdateMessage(const nlohmann::json& msg);

    std::string m_host;
    std::string m_port;

    net::io_context m_ioc;
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    std::unique_ptr<net::executor_work_guard<net::io_context::executor_type>> m_work;
    std::thread m_thread;

    // m_sslCtx must be declared before m_ws (initialisation order).
    ssl::context m_sslCtx{ssl::context::tlsv13_client};
    boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::beast::tcp_stream>> m_ws{m_strand, m_sslCtx};
    boost::beast::flat_buffer m_buffer;

    std::deque<std::string> m_writeQueue;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_isConnected{false};
};

Q_DECLARE_METATYPE(SentinelStreamClient::HeatmapHistoryColumn)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::HeatmapHistoryColumn>)
Q_DECLARE_METATYPE(SentinelStreamClient::CandleBar)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::CandleBar>)
