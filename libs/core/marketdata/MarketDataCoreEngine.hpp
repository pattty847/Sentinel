#pragma once
#include <memory>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "auth/Authenticator.hpp"
#include "ws/SubscriptionManager.hpp"
#include "ws/BeastWsTransport.hpp"
#include "model/TradeData.h"
#include "../config/ConfigTypes.hpp"

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class MarketDataCoreEngine : public std::enable_shared_from_this<MarketDataCoreEngine> {
public:
    using TradeCb = std::function<void(const Trade&)>;
    using OrderBookLevelUpdatesCb = std::function<void(const std::string&,
                                                       const std::vector<BookLevelUpdate>&,
                                                       int64_t exchangeMs)>;
    using OrderBookInitializedCb = std::function<void(const std::string&,
                                                      const std::vector<OrderBookLevel>&,
                                                      const std::vector<OrderBookLevel>&,
                                                      int64_t envelopeMs)>;
    // The book for this connection's product is no longer trustworthy:
    // disconnect, sequence gap or malformed L2. It becomes valid again only at
    // the next snapshot for that product.
    using OrderBookInvalidatedCb = std::function<void(const std::string& productId,
                                                      const std::string& reason)>;
    using ConnectionStatusCb = std::function<void(const std::string&, bool)>;
    using ErrorCb = std::function<void(const std::string&, const std::string&)>;
    using LatencyCb = std::function<void(int)>;

    // Optional pre-parse capture tap. Views are valid only for the callback; set
    // before start(). Runs on the I/O thread. No clocks/copies when unset.
    enum class IngestKind { Frame, TransportUp, TransportDown, BookInvalidated, ResyncRequested };
    struct IngestObservation {
        IngestKind kind;
        uint64_t connection;
        int64_t systemNs;
        int64_t steadyNs;
        std::string_view payload;
        std::string_view product;
        std::string_view reason;
    };
    using IngestObserver = std::function<void(const IngestObservation&)>;
    void onIngest(IngestObserver cb) { m_ingestObserver = std::move(cb); }

    struct ReconnectPolicy {
        std::chrono::milliseconds initialDelay{1000};
        std::chrono::milliseconds maximumDelay{30000};
        std::chrono::milliseconds watchdogInterval{2000};
        std::chrono::milliseconds heartbeatStale{20000};
        std::chrono::milliseconds staleHeartbeatDelay{5000};
        std::chrono::milliseconds resnapshotCooldown{20000};
        // Per-product silence/recovery intervals; constructor-only test overrides.
        std::chrono::milliseconds level2Stale{30000};
        std::chrono::milliseconds level2RetryMaximum{600000};
    };
    using TransportFactory = std::function<std::unique_ptr<WsTransport>(const std::string&, net::io_context&, ssl::context&)>;
    using Clock = std::function<int64_t()>; // monotonic microseconds
    using ConnectPermit = std::function<bool(int64_t)>;
    using Jitter = std::function<std::chrono::milliseconds()>;
    struct Stats {
        std::string product;
        bool up = false;
        uint64_t connection = 0, reconnects = 0;
        int64_t sequence = -1, lastMessageAgeMs = -1, level2AgeMs = -1;
    };
    MarketDataCoreEngine(Authenticator&, const ServerMdcConfig&, std::string product,
                         net::io_context&, ssl::context&, TransportFactory, ReconnectPolicy,
                         Clock, ConnectPermit, Jitter);
    ~MarketDataCoreEngine();
    // Owner calls these on the shared I/O thread. Each mutation enters this engine's strand.
    void start();
    void stop(std::function<void()> completion);
    void tick();
    void requestResnapshot();
    Stats stats() const; // shared I/O thread only

    MarketDataCoreEngine(const MarketDataCoreEngine&) = delete;
    MarketDataCoreEngine& operator=(const MarketDataCoreEngine&) = delete;
    MarketDataCoreEngine(MarketDataCoreEngine&&) = delete;
    MarketDataCoreEngine& operator=(MarketDataCoreEngine&&) = delete;

    void onTrade(TradeCb cb) { m_onTrade = std::move(cb); }
    void onLiveOrderBookLevelUpdates(OrderBookLevelUpdatesCb cb) { m_onLiveOrderBookLevelUpdates = std::move(cb); }
    void onLiveOrderBookInitialized(OrderBookInitializedCb cb) { m_onLiveOrderBookInitialized = std::move(cb); }
    void onLiveOrderBookInvalidated(OrderBookInvalidatedCb cb) { m_onLiveOrderBookInvalidated = std::move(cb); }
    void onConnectionStatus(ConnectionStatusCb cb) { m_onConnectionStatus = std::move(cb); }
    void onError(ErrorCb cb) { m_onError = std::move(cb); }
    void onLatency(LatencyCb cb) { m_onLatency = std::move(cb); }

private:
    void observeIngest(IngestKind kind, std::string_view payload = {},
                       std::string_view reason = {}) noexcept;
    void scheduleReconnect(bool initial = false);
    void transportStatus(bool up);
    void receive(std::string payload);

    void sendSubscriptions();
    void checkLevel2Silence(int64_t nowMs);
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
    void reconnectNow(const std::string& reason); // strand only

    void emitError(std::string msg);
    void emitConnectionStatus(bool connected);
    void emitBookInvalidated(const std::string& reason);

    std::string                     m_host;
    std::string                     m_port;
    std::string                     m_target;
    bool                            m_useJwt = false;
    const std::string m_product;
    Authenticator& m_auth;
    net::strand<net::io_context::executor_type> m_strand;
    std::shared_ptr<WsTransport> m_transport;
    ReconnectPolicy m_reconnectPolicy;
    Clock m_clock;
    ConnectPermit m_connectPermit;
    Jitter m_jitter;
    bool m_running = false, m_connected = false;
    bool m_reconnectScheduled = false, m_closePending = false;
    std::chrono::milliseconds m_backoffDuration;
    int64_t m_connectAt = 0, m_watchdogAt = 0;
    int64_t m_lastHeartbeatMs = -1, m_lastSequenceNum = -1, m_lastResnapshotMs = -1;
    uint64_t m_connection = 0;
    bool m_loggedEmptySubscriptionAck = false, m_warnedMissingLevel2 = false;
    struct ProductLiveness {
        int64_t lastLevel2Ms = 0, retryMs = 0;
        bool snapshotAccepted = false;
    } m_liveness;
    std::vector<BookLevelUpdate> m_levelUpdates;
    std::function<void()> m_stopCompletion;

    TradeCb                          m_onTrade;
    OrderBookLevelUpdatesCb          m_onLiveOrderBookLevelUpdates;
    OrderBookInitializedCb           m_onLiveOrderBookInitialized;
    OrderBookInvalidatedCb           m_onLiveOrderBookInvalidated;
    ConnectionStatusCb               m_onConnectionStatus;
    ErrorCb                          m_onError;
    LatencyCb                        m_onLatency;
    IngestObserver                   m_ingestObserver;
};
