/*
Sentinel — MarketDataCore
Role: Manages the primary WebSocket connection for real-time market data streams.
Inputs/Outputs: Ingests JSON from WebSocket; produces Trade/OrderBook data for DataCache.
Threading: Runs network I/O on a dedicated worker thread; safely dispatches to main thread via Qt signals.
Performance: Hot path is message parsing; uses fast string conversion and throttled logging.
Integration: Instantiated by main app; feeds DataCache and GUI via tradeReceived/orderBookUpdated signals.
Observability: Logs connection lifecycle and errors via SentinelLogging; data path logging is throttled.
Related: MarketDataCore.hpp, DataCache.hpp, Authenticator.hpp, TradeData.h.
Assumptions: Authenticator and DataCache instances outlive this object; API is Coinbase-like.
*/
#include "MarketDataCore.hpp"
#include "SentinelLogging.hpp"
#include "dispatch/MessageDispatcher.hpp"
#include "dispatch/Channels.hpp"
#include "Cpp20Utils.hpp"
#include <thread>
#include <chrono>
#include <span>
#include <utility>
#include <QString>
#include <QMetaObject>
#include <QMetaType>
#include <QPointer>
#include <format>    // std::format for efficient string formatting

MarketDataCore::MarketDataCore(Authenticator& auth,
                               DataCache& cache,
                               std::shared_ptr<SentinelMonitor> monitor)
    : QObject(nullptr)
    , m_auth(auth)
    , m_cache(cache)
    , m_monitor(monitor)
{
    qRegisterMetaType<BookDelta>("BookDelta");
    qRegisterMetaType<std::vector<BookDelta>>("BookDeltaVector");

    // Configure SSL context
    m_sslCtx.set_default_verify_paths();
    m_sslCtx.set_verify_mode(ssl::verify_peer);
    
    sLog_App("MarketDataCore initialized");
    // Create transport and bind callbacks
    m_transport = std::make_unique<BeastWsTransport>(m_ioc, m_sslCtx);
    m_transport->onStatus([this](bool up){
        m_connected.store(up);
        if (up) {
            // Reset sequencing and heartbeat tracking on fresh connect
            m_lastSeqByProduct.clear();
            m_lastHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch()).count());
            QPointer<MarketDataCore> self(this);
            QMetaObject::invokeMethod(this, [self]{ if (!self) return; emit self->connectionStatusChanged(true); }, Qt::QueuedConnection);
            replaySubscriptionsOnConnect();
            startHeartbeatWatchdog();
            sendHeartbeatSubscribe();
        } else {
            emitError("Transport down");
        }
    });
    m_transport->onError([this](std::string err){ emitError(QString::fromStdString(err)); });
    m_transport->onMessage([this](std::string payload){
        try {
            auto j = nlohmann::json::parse(payload);
            dispatch(j);
        } catch (const nlohmann::json::parse_error& e) {
            sLog_Error(QString("JSON parse error in transport message: %1").arg(e.what()));
        } catch (const std::exception& e) {
            sLog_Error(QString("Error processing transport message: %1").arg(e.what()));
        }
    });
}

MarketDataCore::~MarketDataCore() {
    stop();
    sLog_App("MarketDataCore destroyed");
}

inline void MarketDataCore::emitError(QString msg) {
    QPointer<MarketDataCore> self(this);
    QMetaObject::invokeMethod(this, [self, m = std::move(msg)]{
        if (!self) return;
        emit self->errorOccurred(m);
        emit self->connectionStatusChanged(false);
    }, Qt::QueuedConnection);
}

void MarketDataCore::subscribeToSymbols(const std::vector<std::string>& symbols) {    
    std::vector<std::string> new_symbols;
    for (const auto& s : symbols) {
        if (std::find(m_products.begin(), m_products.end(), s) == m_products.end()) {
            m_products.push_back(s);
            new_symbols.push_back(s);
        }
    }
    if (!new_symbols.empty()) {
        // Keep subscription manager in sync
        m_subscriptions.setDesiredProducts(m_products);
    }
    if (!new_symbols.empty()) {
        sendSubscriptionMessage("subscribe", new_symbols);
    }
}

void MarketDataCore::unsubscribeFromSymbols(const std::vector<std::string>& symbols) {
    std::vector<std::string> removed_symbols;
    for (const auto& s : symbols) {
        auto it = std::find(m_products.begin(), m_products.end(), s);
        if (it != m_products.end()) {
            m_products.erase(it);
            removed_symbols.push_back(s);
        }
    }
    if (!removed_symbols.empty()) {
        m_subscriptions.setDesiredProducts(m_products);
        sendSubscriptionMessage("unsubscribe", removed_symbols);
    }
}

void MarketDataCore::start() {
    if (!m_running.exchange(true)) {
        sLog_App("Starting MarketDataCore...");
        
        // Reset backoff on fresh start
        m_backoffDuration = std::chrono::seconds(1);
        
        // Create work guard to keep io_context alive
        m_workGuard.emplace(m_ioc.get_executor());
        
        // Restart io_context in case it was previously stopped
        m_ioc.restart();
        
        // Start I/O thread
        m_ioThread = std::thread(&MarketDataCore::run, this);
        if (m_transport) {
            m_transport->connect(m_host, m_port, m_target);
        }
    }
}

void MarketDataCore::stop() {
    if (m_running.exchange(false)) {
        sLog_App("Stopping MarketDataCore...");

        // Cancel reconnect timer
        m_reconnectTimer.cancel();

        if (m_transport) m_transport->close();

        // Release work guard to allow io_context to exit
        m_workGuard.reset();

        // Stop io_context to unblock the I/O thread
        m_ioc.stop();

        // Join thread
        if (m_ioThread.joinable()) {
            m_ioThread.join();
        }

        sLog_App("MarketDataCore stopped");
    }
}

void MarketDataCore::run() {
    sLog_Data(QString("IO context running for transport (%1:%2)").arg(QString::fromStdString(m_host)).arg(QString::fromStdString(m_port)));
    
    // Transport handles resolve/connect/handshake; we just run the context
    m_ioc.run();
    
    sLog_Data("IO context stopped");
}
void MarketDataCore::scheduleReconnect() {
    if (!m_running) return;
    
    // Exponential backoff with jitter (max 60s)
    m_backoffDuration = std::min(m_backoffDuration * 2, std::chrono::seconds(60));
    
    // Add 0-250ms jitter to prevent thundering herd
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> jitter(0, 250);
    auto delay = m_backoffDuration + std::chrono::milliseconds(jitter(gen));
    
    sLog_Data(QString("Scheduling reconnect in %1ms (backoff: %2s)...")
        .arg(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count())
        .arg(m_backoffDuration.count()));
    
    // NON-BLOCKING timer-based reconnect
    m_reconnectTimer.expires_after(delay);
    m_reconnectTimer.async_wait([this](beast::error_code ec) {
        if (ec || !m_running) return;
        
        sLog_Data("Attempting reconnection...");
        
        // Record network reconnection
        if (m_monitor) {
            m_monitor->recordNetworkReconnect();
        }
        if (m_transport) {
            m_transport->close();
            m_transport->connect(m_host, m_port, m_target);
        }
    });
}

void MarketDataCore::sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        return;
    }

    // Post to the strand to ensure thread-safe access to the WebSocket stream
    net::post(m_strand, [this, type, symbols]() {
        // Stage desired set if we are not connected; replay happens on status=true
        if (!m_connected.load()) {
            sLog_Warning("Transport not connected, staging subscription request for replay on connect.");
            if (type == "subscribe") {
                for (const auto& s : symbols) {
                    if (std::find(m_products.begin(), m_products.end(), s) == m_products.end()) {
                        m_products.push_back(s);
                    }
                }
            } else if (type == "unsubscribe") {
                for (const auto& s : symbols) {
                    auto it = std::find(m_products.begin(), m_products.end(), s);
                    if (it != m_products.end()) m_products.erase(it);
                }
            }
            return;
        }

        // Use SubscriptionManager to build frames deterministically
        m_subscriptions.setDesiredProducts(m_products);
        const std::string jwt = m_auth.createJwt();
        const auto frames = (type == "subscribe") ? m_subscriptions.buildSubscribeMsgs(jwt)
                                                   : m_subscriptions.buildUnsubscribeMsgs(jwt);
        if (m_transport && m_connected.load()) {
            for (const auto& frame : frames) {
                m_transport->send(frame);
            }
            sLog_Data("📤 Sent subscription frames via transport");
        }
    });
}
void MarketDataCore::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;
    
    // Record message arrival time for latency analysis
    auto arrival_time = std::chrono::system_clock::now();
    
    std::string channel = message.value("channel", "");
    // Consider any incoming message as liveness to avoid premature reconnection before first heartbeat arrives
    m_lastHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
    if (channel == ch::kHeartbeats) {
        handleHeartbeats(message);
        return;
    }
    
    // Minimal dispatcher wiring for non-data events (ack/errors)
    // Keep existing hot-path handlers for trades and order book intact.
    {
        auto result = MessageDispatcher::parse(message);
        for (const auto& evt : result.events) {
            std::visit([this](auto&& ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, ProviderErrorEvent>) {
                    emitError(QString::fromStdString(ev.message));
                } else if constexpr (std::is_same_v<T, SubscriptionAckEvent>) {
                    std::string logMessage = std::format("Subscription confirmed for {} symbols", ev.productIds.size());
                    sLog_Data(QString::fromStdString(logMessage));
                }
            }, evt);
        }
    }
    
    if (channel == ch::kTrades) {
        handleMarketTrades(message, arrival_time);
    } else if (channel == ch::kL2Data) {
        handleOrderBookData(message, arrival_time);
    }
}

void MarketDataCore::handleMarketTrades(const nlohmann::json& message, 
                                      const std::chrono::system_clock::time_point& arrival_time) {
    if (!message.contains("events")) return;
    
    for (const auto& event : message["events"]) {
        if (event.contains("trades")) {
            processTrades(event["trades"], arrival_time);
        }
    }
}

void MarketDataCore::processTrades(const nlohmann::json& trades,
                                 const std::chrono::system_clock::time_point& arrival_time) {
    for (const auto& trade_data : trades) {
        Trade trade = createTradeFromJson(trade_data, arrival_time);
        
        // Store through sink adapter
        m_sink.onTrade(trade);
        
        // Record trade processed for throughput tracking
        if (m_monitor) {
            m_monitor->recordTradeProcessed(trade);
        }
        
        // Emit real-time signal to GUI layer (Qt thread-safe)
        Trade tradeCopy = trade; // Make a copy for thread safety
        {
            QPointer<MarketDataCore> self(this);
            QMetaObject::invokeMethod(this, [self, tradeCopy]() {
                if (!self) return;
                emit self->tradeReceived(tradeCopy);
            }, Qt::QueuedConnection);
        }
        
        std::string logMessage = Cpp20Utils::formatTradeLog(
            trade.product_id, trade.price, trade.size, 
            trade.side == AggressorSide::Buy ? "buy" : "sell", 
            m_tradeLogCount.load());
        sLog_Data(QString::fromStdString(logMessage));
    }
}

Trade MarketDataCore::createTradeFromJson(const nlohmann::json& trade_data,
                                        const std::chrono::system_clock::time_point& arrival_time) {
    Trade trade;
    trade.product_id = trade_data.value("product_id", "");
    trade.trade_id = trade_data.value("trade_id", "");
    trade.price = Cpp20Utils::fastStringToDouble(trade_data.value("price", "0"));
    trade.size = Cpp20Utils::fastStringToDouble(trade_data.value("size", "0"));
    
    // Parse side
    const std::string side = trade_data.value("side", "");
    trade.side = Cpp20Utils::fastSideDetection(side);
    
    // Parse timestamp from trade data or use exchange timestamp
    if (trade_data.contains("time")) {
        std::string trade_timestamp_str = trade_data["time"];
        trade.timestamp = Cpp20Utils::parseISO8601(trade_timestamp_str);
        
        // Record trade latency (exchange → arrival)
        if (m_monitor) {
            m_monitor->recordTradeLatency(trade.timestamp, arrival_time);
        }
    } else {
        trade.timestamp = std::chrono::system_clock::now();
    }
    
    return trade;
}

void MarketDataCore::handleOrderBookData(const nlohmann::json& message,
                                       const std::chrono::system_clock::time_point& arrival_time) {
    // Sequence number at message root
    uint64_t seq = 0;
    if (message.contains("sequence_num")) {
        try { seq = message["sequence_num"].get<uint64_t>(); } catch (...) { seq = 0; }
    }
    // Parse exchange timestamp from root-level JSON
    std::chrono::system_clock::time_point exchange_timestamp = std::chrono::system_clock::now();
    if (message.contains("timestamp")) {
        // Parse ISO8601 timestamp: "2023-02-09T20:32:50.714964855Z"
        std::string timestamp_str = message["timestamp"];
        exchange_timestamp = Cpp20Utils::parseISO8601(timestamp_str);
        
        // Record order book latency (exchange → arrival)
        if (m_monitor) {
            m_monitor->recordOrderBookLatency(exchange_timestamp, arrival_time);
        }
    }
    
    if (!message.contains("events")) return;
    
    for (const auto& event : message["events"]) {
        std::string eventType = event.value("type", "");
        std::string product_id = event.value("product_id", "");
        
        // For l2_data, Coinbase guarantees delivery; do not enforce sequence gating.

        if (eventType == "snapshot") {
            handleOrderBookSnapshot(event, product_id, exchange_timestamp);
        } else if (eventType == "update") {
            handleOrderBookUpdate(event, product_id, exchange_timestamp);
        }
    }
}

void MarketDataCore::handleOrderBookSnapshot(const nlohmann::json& event,
                                           const std::string& product_id,
                                           const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
    
    // SNAPSHOT: Initialize complete order book state
    std::vector<OrderBookLevel> sparse_bids;
    std::vector<OrderBookLevel> sparse_asks;
    
    // Process snapshot data into temporary sparse vectors
    for (const auto& update : event["updates"]) {
        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
            continue;
        }
        
        std::string side = update["side"];
        double price = Cpp20Utils::fastStringToDouble(update["price_level"].get<std::string>());
        double quantity = Cpp20Utils::fastStringToDouble(update["new_quantity"].get<std::string>());
        
        if (quantity > 0.0) {
            OrderBookLevel level = {price, quantity};
            if (side == "bid") {
                sparse_bids.push_back(level);
            } else if (side_norm::normalize(side) == "ask") {
                sparse_asks.push_back(level);
            }
        }
    }
    
    // Initialize the live order book with the sparse snapshot data
    m_cache.initializeLiveOrderBook(product_id, sparse_bids, sparse_asks, exchange_timestamp);
    
    std::string logMessage = Cpp20Utils::formatOrderBookLog(
        product_id, sparse_bids.size(), sparse_asks.size());
    sLog_Data(QString::fromStdString(logMessage));
}

void MarketDataCore::handleOrderBookUpdate(const nlohmann::json& event,
                                         const std::string& product_id,
                                         const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
    
    // UPDATE: Apply incremental changes to stateful order book
    thread_local std::vector<BookLevelUpdate> levelUpdates;
    levelUpdates.clear();
    levelUpdates.reserve(event["updates"].size());

    for (const auto& update : event["updates"]) {
        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
            continue;
        }

        std::string side = update["side"];
        if (side == "offer") {
            side = "ask";
        }

        const bool isBid = (side == "bid");
        if (!isBid && side != "ask") {
            continue;
        }

        double price = Cpp20Utils::fastStringToDouble(update["price_level"].get<std::string>());
        double quantity = Cpp20Utils::fastStringToDouble(update["new_quantity"].get<std::string>());

        levelUpdates.push_back(BookLevelUpdate{isBid, price, quantity});
    }

    thread_local std::vector<BookDelta> deltas;
    if (!levelUpdates.empty()) {
        m_cache.applyLiveOrderBookUpdates(product_id,
                                          std::span<const BookLevelUpdate>(levelUpdates.data(), levelUpdates.size()),
                                          exchange_timestamp,
                                          deltas);
    } else {
        deltas.clear();
    }
    const int updateCount = static_cast<int>(deltas.size());
    
    // Direct dense-only signal - NO CONVERSION!
    std::vector<BookDelta> deltasPayload(deltas.begin(), deltas.end());
    deltas.clear();
    {
        QPointer<MarketDataCore> self(this);
        QString productIdQ = QString::fromStdString(product_id);
        QMetaObject::invokeMethod(this, [self, productIdQ, deltasMove = std::move(deltasPayload)]() mutable {
            if (!self) return;
            emit self->liveOrderBookUpdated(productIdQ, deltasMove);
        }, Qt::QueuedConnection);
    }
    
    // Record order book update metrics
    const auto& liveBook = m_cache.getDirectLiveOrderBook(product_id);
    size_t bidCount = liveBook.getBidCount();
    size_t askCount = liveBook.getAskCount();
    
    if (m_monitor) {
        m_monitor->recordOrderBookUpdate(QString::fromStdString(product_id), bidCount, askCount);
    }
    
    std::string logMessage = Cpp20Utils::formatOrderBookLog(
        product_id, bidCount, askCount, updateCount);
    sLog_Data(QString::fromStdString(logMessage));
}

void MarketDataCore::replaySubscriptionsOnConnect() {
    if (m_products.empty()) return;
    auto symbols = m_products;
    sendSubscriptionMessage("subscribe", symbols);
}

void MarketDataCore::handleHeartbeats(const nlohmann::json& message) {
    // Update last heartbeat timestamp; optionally validate counter
    m_lastHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
}

void MarketDataCore::startHeartbeatWatchdog() {
    // Run periodic checks on the strand
    net::post(m_strand, [this](){
        m_heartbeatTimer.expires_after(std::chrono::seconds(2));
        m_heartbeatTimer.async_wait([this](beast::error_code ec){
            if (ec || !m_running.load()) return;
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t lastMs = m_lastHeartbeatMs.load();
            if (lastMs > 0 && (nowMs - lastMs) > 10000) {
                sLog_Error("Heartbeat stale (>10s); reconnecting...");
                triggerImmediateReconnect("stale heartbeat");
                return; // watchdog will be restarted on connect
            }
            // reschedule
            startHeartbeatWatchdog();
        });
    });
}

void MarketDataCore::triggerImmediateReconnect(const char* reason) {
    net::post(m_strand, [this, r = std::string(reason)](){
        sLog_Data(QString("Immediate reconnect: %1").arg(QString::fromStdString(r)));
        // Reset backoff and cancel any pending reconnect
        m_backoffDuration = std::chrono::seconds(1);
        m_reconnectTimer.cancel();
        if (m_transport) {
            m_transport->close();
            // Use standard backoff-based reconnect to avoid transport state races
            scheduleReconnect();
        }
    });
}

int MarketDataCore::checkAndTrackSequence(const std::string& product_id, uint64_t seq, bool isSnapshot) {
    // For Advanced Trade l2_data, delivery is guaranteed; treat sequence as informational only.
    // Keep latest observed sequence per product for diagnostics, but never gate processing.
    if (isSnapshot) {
        m_lastSeqByProduct[product_id] = seq;
        return 0;
    }
    auto it = m_lastSeqByProduct.find(product_id);
    if (it == m_lastSeqByProduct.end() || seq >= it->second) {
        m_lastSeqByProduct[product_id] = seq;
    }
    return 0;
}

void MarketDataCore::sendHeartbeatSubscribe() {
    net::post(m_strand, [this]() {
        if (!m_connected.load() || !m_transport) return;
        nlohmann::json msg;
        msg["type"] = "subscribe";
        msg["channel"] = ch::kHeartbeats;
        // Heartbeats do not require product_ids
        msg["jwt"] = m_auth.createJwt();
        m_transport->send(msg.dump());
        sLog_Data("📤 Subscribed to heartbeats");
    });
}
