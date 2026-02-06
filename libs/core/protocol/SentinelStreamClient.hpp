#pragma once
#include <QObject>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
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
#include "../marketdata/model/TradeData.h"
#include "../config/ConfigTypes.hpp"

namespace net = boost::asio;
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

    explicit SentinelStreamClient(const std::string& host, const std::string& port, QObject* parent = nullptr);
    ~SentinelStreamClient();

    void connectToServer();
    void disconnectFromServer();
    
    void subscribe(const std::string& symbol);
    void unsubscribe(const std::string& symbol);
    void requestHeatmapHistory(const std::string& symbol,
                               int64_t timeframeMs,
                               int64_t endTimeMs,
                               int count);
    void requestCandleHistory(const std::string& symbol,
                              int64_t timeframeSec,
                              int64_t endTimeSec,
                              int limit);

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

private:
    void run();
    void onResolve(boost::beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(boost::beast::error_code ec, tcp::endpoint ep);
    void onHandshake(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytes_transferred);
    
    void handleMessage(const std::string& msg);

    std::string m_host;
    std::string m_port;
    
    net::io_context m_ioc;
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    std::unique_ptr<net::executor_work_guard<net::io_context::executor_type>> m_work;
    std::thread m_thread;
    
    boost::beast::websocket::stream<boost::beast::tcp_stream> m_ws;
    boost::beast::flat_buffer m_buffer;
    
    std::deque<std::string> m_writeQueue;
    
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_isConnected{false};
};

Q_DECLARE_METATYPE(SentinelStreamClient::HeatmapHistoryColumn)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::HeatmapHistoryColumn>)
Q_DECLARE_METATYPE(SentinelStreamClient::CandleBar)
Q_DECLARE_METATYPE(QVector<SentinelStreamClient::CandleBar>)
