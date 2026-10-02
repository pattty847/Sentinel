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

class MarketDataCoreEngine {
public:
    using TradeCb = std::function<void(const Trade&)>;
    using OrderBookLevelUpdatesCb = std::function<void(const std::string&,
                                                       const std::vector<BookLevelUpdate>&,
                                                       int64_t exchangeMs)>;
    using OrderBookInitializedCb = std::function<void(const std::string&,
                                                      const std::vector<OrderBookLevel>&,
                                                      const std::vector<OrderBookLevel>&,
                                                      int64_t envelopeMs)>;
    // The book for productId (empty = every product) is no longer trustworthy:
    // disconnect, sequence gap or malformed L2. It becomes valid again only at
    // the next snapshot for that product.
    using OrderBookInvalidatedCb = std::function<void(const std::string& productId,
                                                      const std::string& reason)>;
    using ConnectionStatusCb = std::function<void(bool)>;
    using ErrorCb = std::function<void(const std::string&)>;
    using LatencyCb = std::function<void(int)>;

    // Optional pre-parse capture tap. Views are valid only for the callback; set
    // before start(). Runs on the I/O thread. No clocks/copies when unset.
    enum class IngestKind { Frame, TransportUp, TransportDown, BookInvalidated, ResyncRequested };
    struct IngestObservation {
        IngestKind kind;
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
        // requestResnapshot() ignores requests this soon after its last reconnect,
        // whichever product asked: one stuck consumer must not keep gapping the rest.
        std::chrono::milliseconds resnapshotCooldown{20000};
        // Per-product silence/recovery intervals; constructor-only test overrides.
        std::chrono::milliseconds level2Stale{30000};
        std::chrono::milliseconds level2RetryMaximum{600000};
        std::chrono::milliseconds level2QuietMaximum{300000};
    };
    // Alternate transport/timings support deterministic offline tests. All
    // transport callbacks must run on the supplied I/O context's single thread.
    using TransportFactory = std::function<std::unique_ptr<WsTransport>(net::io_context&, ssl::context&)>;
    explicit MarketDataCoreEngine(Authenticator& auth, const ServerMdcConfig& config);
    MarketDataCoreEngine(Authenticator& auth, const ServerMdcConfig& config,
                         TransportFactory transportFactory, ReconnectPolicy policy);


    ~MarketDataCoreEngine();
    void start();
    void stop();

    // Subscription Management
    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);
    // A consumer lost its book for productId on its own (not via onLiveOrderBookInvalidated)
    // and needs a fresh snapshot: invalidate every book (ordered with accepted frames),
    // then reconnect, which resubscribes every product. Thread-safe; ignored while
    // disconnected or reconnecting, and within resnapshotCooldown of the last one.
    void requestResnapshot(const std::string& productId);

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
                       std::string_view product = {}, std::string_view reason = {}) noexcept;
    void run();
    void scheduleReconnect();

    // Strand only; public subscription changes post the entire mutation here.
    void sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols,
                                 bool level2Only = false);
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
    void startHeartbeatWatchdog();
    void triggerImmediateReconnect(const char* reason);
    void reconnectNow(const std::string& reason); // strand only

    void emitError(std::string msg);
    void emitConnectionStatus(bool connected);
    void emitBookInvalidated(const std::string& productId, const std::string& reason);

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
    std::unique_ptr<WsTransport>      m_transport;
    
    std::atomic<bool>               m_running{false};
    std::atomic<bool>               m_connected{false};
    ReconnectPolicy                m_reconnectPolicy;
    std::chrono::milliseconds       m_backoffDuration{1000};
    // I/O-thread-owned. Duplicate down/close callbacks share one pending retry.
    bool                            m_reconnectScheduled = false;
    bool                            m_closePending = false;
    std::thread                     m_ioThread;
    
    std::atomic<int>                m_tradeLogCount{0};
    std::atomic<int>                m_orderBookLogCount{0};
    std::unordered_map<std::string, uint64_t> m_lastSeqByProduct;
    std::mutex                      m_seqMutex;
    std::atomic<int64_t>            m_lastHeartbeatMs{0};
    // Coinbase sequence_num is per connection and contiguous across every
    // channel, starting at 0 (measured 2026-09-28). Only touched on the io strand.
    int64_t                         m_lastSequenceNum = -1;
    int64_t                         m_lastResnapshotMs = -1; // steady ms; io strand only
    bool                            m_loggedEmptySubscriptionAck = false;
    struct ProductLiveness {
        int64_t lastLevel2Ms;
        int64_t resubscribeMs = -1; // cleared only by a valid snapshot
        int64_t retryMs = 0, quietMs = 0;
        unsigned failures = 0, reconnectEscalations = 0;
        bool snapshotAccepted = false;
        // Bounded snapshot evidence, not a shadow book. An update invalidates
        // the comparison baseline; only consecutive quiet snapshots compare.
        bool comparableSnapshot = false;
        uint64_t snapshotHash = 0, snapshotHash2 = 0;
        size_t snapshotLevels = 0;
    };
    // Allocated on subscription changes only; find/update on each L2 event.
    // Desired products and liveness are owned by the I/O strand.
    std::unordered_map<std::string, ProductLiveness> m_productLiveness;

    TradeCb                          m_onTrade;
    OrderBookLevelUpdatesCb          m_onLiveOrderBookLevelUpdates;
    OrderBookInitializedCb           m_onLiveOrderBookInitialized;
    OrderBookInvalidatedCb           m_onLiveOrderBookInvalidated;
    ConnectionStatusCb               m_onConnectionStatus;
    ErrorCb                          m_onError;
    LatencyCb                        m_onLatency;
    IngestObserver                   m_ingestObserver;
};
