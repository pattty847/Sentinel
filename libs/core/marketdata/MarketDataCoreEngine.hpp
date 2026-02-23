#pragma once
#include <memory>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <string>
#include <vector>
#include "auth/Authenticator.hpp"
#include "ws/SubscriptionManager.hpp"
#include "ws/BeastWsTransport.hpp"
#include "model/TradeData.h"
#include "../config/ConfigTypes.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class MarketDataCoreEngine {
public:
    using TradeCb = std::function<void(const Trade&)>;
    using OrderBookLevelUpdatesCb = std::function<void(const std::string&,
                                                       const std::vector<BookLevelUpdate>&,
                                                       int64_t exchangeMs)>;
    using OrderBookInitializedCb = std::function<void(const std::string&,
                                                      const std::vector<OrderBookLevel>&,
                                                      const std::vector<OrderBookLevel>&)>;
    using ConnectionStatusCb = std::function<void(bool)>;
    using ErrorCb = std::function<void(const std::string&)>;
    using LatencyCb = std::function<void(int)>;

    explicit MarketDataCoreEngine(Authenticator& auth, const ServerMdcConfig& config);

    ~MarketDataCoreEngine();
    void start();
    void stop();

    // Subscription Management
    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

    MarketDataCoreEngine(const MarketDataCoreEngine&) = delete;
    MarketDataCoreEngine& operator=(const MarketDataCoreEngine&) = delete;
    MarketDataCoreEngine(MarketDataCoreEngine&&) = delete;
    MarketDataCoreEngine& operator=(MarketDataCoreEngine&&) = delete;

    void onTrade(TradeCb cb) { m_onTrade = std::move(cb); }
    void onLiveOrderBookLevelUpdates(OrderBookLevelUpdatesCb cb) { m_onLiveOrderBookLevelUpdates = std::move(cb); }
    void onLiveOrderBookInitialized(OrderBookInitializedCb cb) { m_onLiveOrderBookInitialized = std::move(cb); }
    void onConnectionStatus(ConnectionStatusCb cb) { m_onConnectionStatus = std::move(cb); }
    void onError(ErrorCb cb) { m_onError = std::move(cb); }
    void onLatency(LatencyCb cb) { m_onLatency = std::move(cb); }

private:
    void run();
    void scheduleReconnect();

    void sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols);
    void dispatch(const nlohmann::json&);

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

    void handleHeartbeats(const nlohmann::json& message);
    void startHeartbeatWatchdog();
    void triggerImmediateReconnect(const char* reason);

    void emitError(std::string msg);
    void emitConnectionStatus(bool connected);

    void replaySubscriptionsOnConnect();
    std::string                     m_host;
    std::string                     m_port;
    std::string                     m_target;
    bool                            m_useJwt = false;
    std::string                     m_sslCaBundle;
    std::vector<std::string>        m_products;

    Authenticator&                  m_auth;
    SubscriptionManager             m_subscriptions;

    net::io_context                 m_ioc;
    ssl::context                    m_sslCtx{ssl::context::tlsv12_client};
    net::strand<net::io_context::executor_type> m_strand{m_ioc.get_executor()};
    net::steady_timer               m_reconnectTimer{m_strand};
    net::steady_timer               m_heartbeatTimer{m_strand};
    std::optional<net::executor_work_guard<net::io_context::executor_type>> m_workGuard;
    std::unique_ptr<BeastWsTransport> m_transport;
    
    std::atomic<bool>               m_running{false};
    std::atomic<bool>               m_connected{false};
    std::chrono::seconds            m_backoffDuration{1};
    std::thread                     m_ioThread;
    
    std::atomic<int>                m_tradeLogCount{0};
    std::atomic<int>                m_orderBookLogCount{0};
    std::unordered_map<std::string, uint64_t> m_lastSeqByProduct;
    std::mutex                      m_seqMutex;
    std::atomic<int64_t>            m_lastHeartbeatMs{0};
    bool                            m_loggedEmptySubscriptionAck = false;

    TradeCb                          m_onTrade;
    OrderBookLevelUpdatesCb          m_onLiveOrderBookLevelUpdates;
    OrderBookInitializedCb           m_onLiveOrderBookInitialized;
    ConnectionStatusCb               m_onConnectionStatus;
    ErrorCb                          m_onError;
    LatencyCb                        m_onLatency;
};
