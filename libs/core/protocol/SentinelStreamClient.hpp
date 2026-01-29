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
#include "SentinelStreamProtocol.hpp"
#include "../marketdata/model/TradeData.h"

namespace net = boost::asio;
using tcp = net::ip::tcp;

class SentinelStreamClient : public QObject {
    Q_OBJECT
public:
    explicit SentinelStreamClient(const std::string& host, const std::string& port, QObject* parent = nullptr);
    ~SentinelStreamClient();

    void connectToServer();
    void disconnectFromServer();
    
    void subscribe(const std::string& symbol);
    void unsubscribe(const std::string& symbol);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& error);
    
    void tradeReceived(const Trade& trade);
    // This signal is strictly for internal use by DataSource which converts prices -> indices
    void l2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates);
    
    void liveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);
    void snapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);
    // Other signals as needed for aggregated slices
    void heatmapSliceReceived(const QString& symbol,
                              int64_t bucketStartMs,
                              int64_t bucketEndMs,
                              int64_t timeframeMs,
                              int gridWidth,
                              int gridHeight,
                              double minPrice,
                              double maxPrice,
                              double tickSize,
                              double midPrice,
                              double lastTrade,
                              const QString& format,
                              const QByteArray& column,
                              const QByteArray& liquidityColumn,
                              double liquidityScale,
                              bool reset);

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
