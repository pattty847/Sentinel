// MarketDataCoreEngine: WebSocket market data; I/O on worker thread. Coinbase-like API.
#include "MarketDataCoreEngine.hpp"
#include "SentinelLogging.hpp"
#include "dispatch/MessageDispatcher.hpp"
#include "dispatch/Channels.hpp"
#include "Cpp20Utils.hpp"
#include <thread>
#include <chrono>
#include <span>
#include <algorithm>
#include <random>
#include <utility>
#include <cstdlib>
#include <string>

namespace {
    inline int64_t steadyClockMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static constexpr int64_t kHeartbeatStaleThresholdMs = 10000;
}

MarketDataCoreEngine::MarketDataCoreEngine(Authenticator& auth)
    : m_auth(auth)
{
    if (const char* host = std::getenv("SENTINEL_MDC_HOST")) {
        if (*host) {
            m_host = host;
        }
    }
    if (const char* port = std::getenv("SENTINEL_MDC_PORT")) {
        if (*port) {
            m_port = port;
        }
    }
    if (const char* target = std::getenv("SENTINEL_MDC_TARGET")) {
        if (*target) {
            m_target = target;
        }
    }
    if (const char* useJwt = std::getenv("SENTINEL_MDC_USE_JWT")) {
        if (*useJwt) {
            m_useJwt = (std::string_view(useJwt) == "1");
        }
    }

    // Windows/OpenSSL: prefer SENTINEL_SSL_CA_BUNDLE; default_verify_paths can miss system store.
    if (const char* caBundle = std::getenv("SENTINEL_SSL_CA_BUNDLE")) {
        if (*caBundle) {
            sLog_Data(std::string("Using custom CA bundle from SENTINEL_SSL_CA_BUNDLE: ") + caBundle);
            try {
                m_sslCtx.load_verify_file(caBundle);
            } catch (const std::exception& e) {
                sLog_Error(std::string("Failed to load CA bundle from ") + caBundle + ": " + e.what());
                m_sslCtx.set_default_verify_paths();
            }
        } else {
            m_sslCtx.set_default_verify_paths();
        }
    } else {
        const char* defaultCaBundle = "resources/certs/ca-bundle.crt";
        try {
            sLog_Data(std::string("Using default CA bundle: ") + defaultCaBundle);
            m_sslCtx.load_verify_file(defaultCaBundle);
        } catch (const std::exception& e) {
            sLog_Error(std::string("Failed to load default CA bundle from ") + defaultCaBundle + ": " + e.what());
            m_sslCtx.set_default_verify_paths();
        }
    }
    m_sslCtx.set_verify_mode(ssl::verify_peer);
    
    sLog_App("MarketDataCore initialized");
    m_transport = std::make_unique<BeastWsTransport>(m_ioc, m_sslCtx);
    m_transport->onStatus([this](bool up){
        m_connected.store(up);
        sLog_DataN(1, std::string("WebSocket transport status changed: ") + (up ? "UP" : "DOWN"));
        if (up) {
            m_lastSeqByProduct.clear();
            m_lastHeartbeatMs.store(steadyClockMs());
            {
                std::lock_guard<std::mutex> lock(m_seqMutex);
                m_lastSeqByProduct.clear();
            }
            emitConnectionStatus(true);
            net::post(m_strand, [this]() {
                replaySubscriptionsOnConnect();
            });
            startHeartbeatWatchdog();
            sendHeartbeatSubscribe();
        } else {
            emitError("Transport down");
        }
    });
    m_transport->onError([this](std::string err){ emitError(std::move(err)); });
    m_transport->onMessage([this](std::string payload){
        try {
            auto j = nlohmann::json::parse(payload);
            dispatch(j);
        } catch (const nlohmann::json::parse_error& e) {
            sLog_Error(std::string("JSON parse error in transport message: ") + e.what());
        } catch (const std::exception& e) {
            sLog_Error(std::string("Error processing transport message: ") + e.what());
        }
    });
}

MarketDataCoreEngine::~MarketDataCoreEngine() {
    stop();
    sLog_App("MarketDataCore destroyed");
}

inline void MarketDataCoreEngine::emitError(std::string msg) {
    if (m_onError) {
        try {
            m_onError(msg);
        } catch (const std::exception& e) {
            sLog_Error(std::string("Error callback exception: ") + e.what());
        }
    }
    emitConnectionStatus(false);
}

inline void MarketDataCoreEngine::emitConnectionStatus(bool connected) {
    if (connected) {
        m_loggedEmptySubscriptionAck = false;
    }
    if (m_onConnectionStatus) {
        try {
            m_onConnectionStatus(connected);
        } catch (const std::exception& e) {
            sLog_Error(std::string("Connection status callback exception: ") + e.what());
        }
    }
}

bool MarketDataCoreEngine::isServerModeEnabled() {
    const char* env = std::getenv("SENTINEL_SERVER_MODE");
    return env && *env;
}

void MarketDataCoreEngine::subscribeToSymbols(const std::vector<std::string>& symbols) {
    std::vector<std::string> new_symbols;
    for (const auto& s : symbols) {
        if (std::find(m_products.begin(), m_products.end(), s) == m_products.end()) {
            m_products.push_back(s);
            new_symbols.push_back(s);
        }
    }
    if (!new_symbols.empty()) {
        m_subscriptions.setDesiredProducts(m_products);
        sLog_DataN(1, std::string("subscribeToSymbols: ") +
                          std::to_string(new_symbols.size()) +
                          " new, " +
                          std::to_string(m_products.size()) +
                          " total products");
    }
    if (!new_symbols.empty()) {
        sendSubscriptionMessage("subscribe", new_symbols);
    }
}

void MarketDataCoreEngine::unsubscribeFromSymbols(const std::vector<std::string>& symbols) {
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

void MarketDataCoreEngine::start() {
    if (!m_running.exchange(true)) {
        sLog_App("Starting MarketDataCore...");
        m_backoffDuration = std::chrono::seconds(1);
        m_workGuard.emplace(m_ioc.get_executor());
        m_ioc.restart();
        m_ioThread = std::thread(&MarketDataCoreEngine::run, this);
        if (m_transport) {
            m_transport->connect(m_host, m_port, m_target);
        }
    }
}

void MarketDataCoreEngine::stop() {
    if (m_running.exchange(false)) {
        sLog_App("Stopping MarketDataCore...");
        m_reconnectTimer.cancel();
        if (m_transport) m_transport->close();
        m_workGuard.reset();
        m_ioc.stop();
        if (m_ioThread.joinable()) {
            m_ioThread.join();
        }

        sLog_App("MarketDataCore stopped");
    }
}

void MarketDataCoreEngine::run() {
    // io_context::run() can exit on unhandled handler exception; loop keeps I/O thread alive.
    while (m_running.load()) {
        try {
            m_ioc.run();
            if (m_running.load()) {
                m_ioc.restart();
            }
        } catch (const std::exception& e) {
            sLog_Error(std::string("IO context thread exception: ") + e.what() + " - restarting I/O loop");
            if (m_running.load()) {
                m_ioc.restart();
            }
        } catch (...) {
            sLog_Error("IO context thread unknown exception - restarting I/O loop");
            if (m_running.load()) {
                m_ioc.restart();
            }
        }
    }
}

void MarketDataCoreEngine::scheduleReconnect() {
    if (!m_running) return;
    m_backoffDuration = std::min(m_backoffDuration * 2, std::chrono::seconds(60));
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> jitter(0, 250);
    auto delay = m_backoffDuration + std::chrono::milliseconds(jitter(gen));
    
    sLog_Data(std::string("Scheduling reconnect in ") +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()) +
              "ms (backoff: " +
              std::to_string(m_backoffDuration.count()) +
              "s)...");
    m_reconnectTimer.expires_after(delay);
    m_reconnectTimer.async_wait([this](beast::error_code ec) {
        if (ec || !m_running) return;
        
        sLog_Data("Attempting reconnection...");
        if (m_transport) {
            m_transport->close();
            m_transport->connect(m_host, m_port, m_target);
        }
    });
}

void MarketDataCoreEngine::sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        return;
    }

    auto symbolsCopy = symbols;
    net::post(m_strand, [this, type, symbolsCopy]() {
        if (!m_connected.load()) {
            sLog_Warning("Transport not connected, staging subscription request for replay on connect.");
            if (type == "subscribe") {
                for (const auto& s : symbolsCopy) {
                    if (std::find(m_products.begin(), m_products.end(), s) == m_products.end()) {
                        m_products.push_back(s);
                    }
                }
            } else if (type == "unsubscribe") {
                for (const auto& s : symbolsCopy) {
                    auto it = std::find(m_products.begin(), m_products.end(), s);
                    if (it != m_products.end()) m_products.erase(it);
                }
            }
            return;
        }
        m_subscriptions.setDesiredProducts(m_products);
        std::string jwt;
        if (m_useJwt) {
            try {
                jwt = m_auth.createJwt();
            } catch (const std::exception& e) {
                sLog_Error(std::string("JWT creation failed in subscription handler: ") + e.what());
                emitError(std::string("Failed to create JWT for subscription: ") + e.what());
                return;
            }
        }
        const auto frames = (type == "subscribe") ? m_subscriptions.buildSubscribeMsgs(jwt)
                                                   : m_subscriptions.buildUnsubscribeMsgs(jwt);
        
        if (m_transport && m_connected.load()) {
            for (const auto& frame : frames) {
                try {
                    auto j = nlohmann::json::parse(frame);
                    if (j.contains("jwt")) {
                        j["jwt"] = "<redacted>";
                    }
                    sLog_DataN(1, std::string("WS ") + type + " frame: " + j.dump());
                } catch (const std::exception&) {
                    sLog_DataN(1, std::string("WS ") + type + " frame (raw): " + frame);
                }
                m_transport->send(frame);
            }
        }
    });
}

void MarketDataCoreEngine::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;
    auto arrival_time = std::chrono::system_clock::now();
    
    static std::atomic<int> rawLogCount{0};
    const int count = rawLogCount.load();
    if (count < 5) {
        if (rawLogCount.fetch_add(1) < 5) {
            sLog_Data("MDC RX: " << message.dump());
        }
    }

    std::string channel = message.value("channel", "");
    m_lastHeartbeatMs.store(steadyClockMs());
    if (channel == ch::kHeartbeats) {
        handleHeartbeats(message);
        return;
    }
    {
        auto result = MessageDispatcher::parse(message);
        for (const auto& evt : result.events) {
            std::visit([this, &message](auto&& ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, ProviderErrorEvent>) {
                    sLog_Error("Provider error: " << ev.message << " | raw=" << message.dump());
                    emitError(ev.message);
                } else if constexpr (std::is_same_v<T, SubscriptionAckEvent>) {
                    if (!ev.productIds.empty()) {
                        sLog_DataN(1, std::string("Subscription confirmed for ") +
                                          std::to_string(ev.productIds.size()) +
                                          " symbol(s)");
                    } else if (!m_loggedEmptySubscriptionAck) {
                        m_loggedEmptySubscriptionAck = true;
                        sLog_Data("Subscription confirmed with empty product list; raw payload: "
                                  << message.dump());
                    }
                }
            }, evt);
        }
        if (channel.empty() && result.events.empty()) {
            sLog_Data("MDC unclassified message: " << message.dump());
        }
    }
    
    if (channel == ch::kTrades) {
        handleMarketTrades(message, arrival_time);
    } else if (channel == ch::kL2Data) {
        handleOrderBookData(message, arrival_time);
    }
}

void MarketDataCoreEngine::handleMarketTrades(const nlohmann::json& message,
                                              const std::chrono::system_clock::time_point& arrival_time) {
    if (!message.contains("events")) return;
    
    for (const auto& event : message["events"]) {
        if (event.contains("trades")) {
            processTrades(event["trades"], arrival_time);
        }
    }
}

void MarketDataCoreEngine::processTrades(const nlohmann::json& trades,
                                         const std::chrono::system_clock::time_point& arrival_time) {
    for (const auto& trade_data : trades) {
        Trade trade = createTradeFromJson(trade_data, arrival_time);
        m_tradeLogCount++;
        
        if (m_onTrade) {
            try {
                m_onTrade(trade);
            } catch (const std::exception& e) {
                sLog_Error(std::string("Trade callback exception: ") + e.what());
            }
        }
        
    }
}

Trade MarketDataCoreEngine::createTradeFromJson(const nlohmann::json& trade_data,
                                                const std::chrono::system_clock::time_point& arrival_time) {
    Trade trade;
    trade.product_id = trade_data.value("product_id", "");
    trade.trade_id = trade_data.value("trade_id", "");
    trade.price = Cpp20Utils::fastStringToDouble(trade_data.value("price", "0"));
    trade.size = Cpp20Utils::fastStringToDouble(trade_data.value("size", "0"));
    const std::string side = trade_data.value("side", "");
    trade.side = Cpp20Utils::fastSideDetection(side);
    if (trade_data.contains("time")) {
        std::string trade_timestamp_str = trade_data["time"];
        trade.timestamp = Cpp20Utils::parseISO8601(trade_timestamp_str);
    } else {
        trade.timestamp = std::chrono::system_clock::now();
    }
    
    return trade;
}

void MarketDataCoreEngine::handleOrderBookData(const nlohmann::json& message,
                                               const std::chrono::system_clock::time_point& arrival_time) {
    uint64_t seq = 0;
    if (message.contains("sequence_num")) {
        try {
            seq = message["sequence_num"].get<uint64_t>();
        } catch (const nlohmann::json::exception& e) {
            sLog_Warning(std::string("sequence_num parse issue: ") + e.what());
            seq = 0;
        }
    }
    std::chrono::system_clock::time_point exchange_timestamp = std::chrono::system_clock::now();
    if (message.contains("timestamp")) {
        std::string timestamp_str = message["timestamp"];
        exchange_timestamp = Cpp20Utils::parseISO8601(timestamp_str);
        auto local_time = std::chrono::system_clock::now();
        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            local_time - exchange_timestamp
        ).count();
        // Guard against bogus timestamps skewing UI latency.
        if (m_onLatency && latency_ms >= 0 && latency_ms < 10000) {
            m_onLatency(static_cast<int>(latency_ms));
        }
    }
    
    if (!message.contains("events")) return;
    
    for (const auto& event : message["events"]) {
        std::string eventType = event.value("type", "");
        std::string product_id = event.value("product_id", "");
        if (eventType == "snapshot") {
            handleOrderBookSnapshot(event, product_id, exchange_timestamp);
        } else if (eventType == "update") {
            handleOrderBookUpdate(event, product_id, exchange_timestamp);
        }
    }
}

void MarketDataCoreEngine::handleOrderBookSnapshot(const nlohmann::json& event,
                                                   const std::string& product_id,
                                                   const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
    std::vector<OrderBookLevel> sparse_bids;
    std::vector<OrderBookLevel> sparse_asks;
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
    if (m_onLiveOrderBookInitialized) {
        try {
            m_onLiveOrderBookInitialized(product_id, sparse_bids, sparse_asks);
        } catch (const std::exception& e) {
            sLog_Error(std::string("Order book init callback exception: ") + e.what());
        }
    }
}

void MarketDataCoreEngine::handleOrderBookUpdate(const nlohmann::json& event,
                                                 const std::string& product_id,
                                                 const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
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

    if (!levelUpdates.empty()) {
        const int64_t exchangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            exchange_timestamp.time_since_epoch()).count();
        std::vector<BookLevelUpdate> updatesPayload(levelUpdates.begin(), levelUpdates.end());
        if (m_onLiveOrderBookLevelUpdates) {
            try {
                m_onLiveOrderBookLevelUpdates(product_id, updatesPayload, exchangeMs);
            } catch (const std::exception& e) {
                sLog_Error(std::string("Order book level updates callback exception: ") + e.what());
            }
        }
    }
}

void MarketDataCoreEngine::replaySubscriptionsOnConnect() {
    if (m_products.empty()) return;
    auto symbols = m_products;
    sendSubscriptionMessage("subscribe", symbols);
}

void MarketDataCoreEngine::handleHeartbeats(const nlohmann::json& message) {
    m_lastHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
}

void MarketDataCoreEngine::startHeartbeatWatchdog() {
    net::post(m_strand, [this](){
        m_heartbeatTimer.expires_after(std::chrono::seconds(2));
        m_heartbeatTimer.async_wait([this](beast::error_code ec){
            if (ec || !m_running.load()) return;
            const int64_t nowMs = steadyClockMs();
            const int64_t lastMs = m_lastHeartbeatMs.load();
            if (lastMs > 0 && (nowMs - lastMs) > kHeartbeatStaleThresholdMs) {
                sLog_Error("Heartbeat stale (>10s); reconnecting...");
                triggerImmediateReconnect("stale heartbeat");
                return;
            }
            startHeartbeatWatchdog();
        });
    });
}

void MarketDataCoreEngine::triggerImmediateReconnect(const char* reason) {
    net::post(m_strand, [this, r = std::string(reason)](){
        sLog_Data(std::string("Immediate reconnect: ") + r);
        m_backoffDuration = std::chrono::seconds(1);
        m_reconnectTimer.cancel();
        if (m_transport) {
            m_transport->close();
            scheduleReconnect();
        }
    });
}

int MarketDataCoreEngine::checkAndTrackSequence(const std::string& product_id, uint64_t seq, bool isSnapshot) {
    // l2_data: delivery guaranteed; sequence is diagnostic only, never gate processing.
    std::lock_guard<std::mutex> lock(m_seqMutex);
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

void MarketDataCoreEngine::sendHeartbeatSubscribe() {
    net::post(m_strand, [this]() {
        if (!m_connected.load() || !m_transport) return;
        try {
            nlohmann::json msg;
            msg["type"] = "subscribe";
            msg["channel"] = ch::kHeartbeats;
            if (m_useJwt) {
                msg["jwt"] = m_auth.createJwt();
            }
            const std::string payload = msg.dump();
            if (m_useJwt) {
                nlohmann::json redacted = msg;
                if (redacted.contains("jwt")) {
                    redacted["jwt"] = "<redacted>";
                }
                sLog_DataN(1, std::string("WS subscribe frame: ") + redacted.dump());
            } else {
                sLog_DataN(1, std::string("WS subscribe frame: ") + payload);
            }
            m_transport->send(payload);
        } catch (const std::exception& e) {
            sLog_Error(std::string("sendHeartbeatSubscribe failed: ") + e.what());
        }
    });
}
