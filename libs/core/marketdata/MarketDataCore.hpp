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
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <optional>
#include <QObject>
#include <QTimer>
#include "auth/Authenticator.hpp"
#include "cache/DataCache.hpp"
#include "sinks/DataCacheSinkAdapter.hpp"
#include "ws/SubscriptionManager.hpp"
#include "ws/BeastWsTransport.hpp"
#include "SentinelMonitor.hpp"
#include "model/TradeData.h"

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class MarketDataCore : public QObject {
    Q_OBJECT

public:
    MarketDataCore(Authenticator& auth,
                   DataCache& cache,
                   std::shared_ptr<SentinelMonitor> monitor = nullptr);

    ~MarketDataCore();
    void start();
    void stop();

    // Subscription Management
    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

    // Non-copyable, non-movable (manages thread)
    MarketDataCore(const MarketDataCore&) = delete;
    MarketDataCore& operator=(const MarketDataCore&) = delete;
    MarketDataCore(MarketDataCore&&) = delete;
    MarketDataCore& operator=(MarketDataCore&&) = delete;

signals:
    void tradeReceived(const Trade& trade);
    void liveOrderBookUpdated(const QString& productId);
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString& error);

private:
    // Connection lifecycle
    void run();
    void scheduleReconnect();

    // Helpers
    void sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols);
    void dispatch(const nlohmann::json&);
    
    // (legacy write queue removed)

    // Message handling sub-functions
    void handleMarketTrades(const nlohmann::json& message, 
                          const std::chrono::system_clock::time_point& arrival_time);
    void processTrades(const nlohmann::json& trades,
                     const std::chrono::system_clock::time_point& arrival_time);
    Trade createTradeFromJson(const nlohmann::json& trade_data,
                            const std::chrono::system_clock::time_point& arrival_time);
    void handleOrderBookData(const nlohmann::json& message,
                           const std::chrono::system_clock::time_point& arrival_time);
    void handleOrderBookSnapshot(const nlohmann::json& event,
                               const std::string& product_id,
                               const std::chrono::system_clock::time_point& exchange_timestamp);
    void handleOrderBookUpdate(const nlohmann::json& event,
                             const std::string& product_id,
                             const std::chrono::system_clock::time_point& exchange_timestamp);
    void handleSubscriptionConfirmation(const nlohmann::json& message);
    void handleError(const nlohmann::json& message);

    // Members
    // Unified error emission to GUI and status surface
    void emitError(QString msg);

    // Subscription & heartbeat helpers
    void replaySubscriptionsOnConnect();
    void startHeartbeat();
    void scheduleNextPing();
    const std::string               m_host   = "advanced-trade-ws.coinbase.com";
    const std::string               m_port   = "443";
    const std::string               m_target = "/";
    std::vector<std::string>        m_products;

    Authenticator&                  m_auth;
    DataCache&                      m_cache;
    DataCacheSinkAdapter            m_sink{m_cache};
    SubscriptionManager             m_subscriptions;
    std::shared_ptr<SentinelMonitor> m_monitor;

    net::io_context                 m_ioc;
    ssl::context                    m_sslCtx{ssl::context::tlsv12_client};
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    // (legacy resolver/websocket/buffer removed; transport owns I/O)
    net::steady_timer               m_reconnectTimer{m_strand};
    net::steady_timer               m_pingTimer{m_strand};
    std::optional<net::executor_work_guard<net::io_context::executor_type>> m_workGuard;
    std::unique_ptr<BeastWsTransport> m_transport; // Phase 3: not yet used for I/O
    
    std::atomic<bool>               m_running{false};
    std::atomic<bool>               m_connected{false};
    std::chrono::seconds            m_backoffDuration{1};
    std::thread                     m_ioThread;
    
    // Thread-safe counters (no more static!)
    std::atomic<int>                m_tradeLogCount{0};
    std::atomic<int>                m_orderBookLogCount{0};
    
    // 🗑️ CLEANED UP: Redundant mutexes removed - write queue handles serialization
    
    // (legacy write queue removed)
};