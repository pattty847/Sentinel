#pragma once
/*
Sentinel — MarketDataCore
Role: Owns the WebSocket connection and I/O thread, handling all network operations.
Inputs/Outputs: Takes product IDs, an Authenticator, and a DataCache; emits parsed data via Qt signals.
Threading: Runs a Boost.Asio io_context on a dedicated worker thread, using a strand for safety.
Performance: High-throughput design using asynchronous I/O for non-blocking operations.
Integration: Created and owned by CoinbaseStreamClient; its signals are wired to the GUI layer.
Observability: Emits connectionStatusChanged and errorOccurred signals.
Related: MarketDataCore.cpp, CoinbaseStreamClient.hpp, DataCache.hpp, Authenticator.hpp.
Assumptions: The provided Authenticator and DataCache instances will outlive this object.
*/
#include <memory>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <optional>
#include <random>
#include <QObject>
#include <QTimer>
#include "Authenticator.hpp"
#include "DataCache.hpp"
#include "TradeData.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class MarketDataCore : public QObject {
    Q_OBJECT

public:
    MarketDataCore(const std::vector<std::string>& products,
                   Authenticator& auth,
                   DataCache& cache);

    ~MarketDataCore();                   // RAII shutdown
    void start();
    void stop();

    // Non-copyable, non-movable (manages thread)
    MarketDataCore(const MarketDataCore&) = delete;
    MarketDataCore& operator=(const MarketDataCore&) = delete;
    MarketDataCore(MarketDataCore&&) = delete;
    MarketDataCore& operator=(MarketDataCore&&) = delete;

signals:
    void tradeReceived(const Trade& trade);
    void orderBookUpdated(std::shared_ptr<const OrderBook> orderBook);
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);

private:
    // Connection/handshake chain
    void run();
    void onResolve(beast::error_code, tcp::resolver::results_type);
    void onConnect(beast::error_code, tcp::resolver::results_type::endpoint_type);
    void onSslHandshake(beast::error_code);
    void onWsHandshake(beast::error_code);
    void onWrite(beast::error_code, std::size_t);
    void onRead(beast::error_code, std::size_t);
    void doClose();
    void onClose(beast::error_code);
    void scheduleReconnect();

    // Helpers
    [[nodiscard]] std::string buildSubscribe(const std::string& channel) const;
    void dispatch(const nlohmann::json&);

    // Members
    const std::string               m_host   = "advanced-trade-ws.coinbase.com";
    const std::string               m_port   = "443";
    const std::string               m_target = "/";
    std::vector<std::string>        m_products;

    Authenticator&                  m_auth;
    DataCache&                      m_cache;

    net::io_context                 m_ioc;
    ssl::context                    m_sslCtx{ssl::context::tlsv12_client};
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    tcp::resolver                   m_resolver{m_strand};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>
                                    m_ws{m_strand, m_sslCtx};
    beast::flat_buffer              m_buf;
    net::steady_timer               m_reconnectTimer{m_strand};
    std::optional<net::executor_work_guard<net::io_context::executor_type>> m_workGuard;
    
    std::atomic<bool>               m_running{false};
    std::chrono::seconds            m_backoffDuration{1};
    std::thread                     m_ioThread;
    
    // Thread-safe counters (no more static!)
    std::atomic<int>                m_tradeLogCount{0};
    std::atomic<int>                m_orderBookLogCount{0};
};