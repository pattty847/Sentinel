// MarketDataCoreEngine: WebSocket market data; I/O on worker thread. Coinbase-like API.
#include "MarketDataCoreEngine.hpp"
#include "SentinelLogging.hpp"
#include "dispatch/MessageDispatcher.hpp"
#include "dispatch/Channels.hpp"
#include "Cpp20Utils.hpp"
#include <thread>
#include <cmath>
#include <limits>
#include <chrono>
#include <algorithm>
#include <random>
#include <utility>
#include <string>

namespace {
    inline int64_t steadyClockMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // Coinbase can be slow to send first frames; 20s avoids aggressive reconnect loop.
    static constexpr int64_t kHeartbeatStaleThresholdMs = 20000;

    // "A,B,C" for log lines.
    std::string joinSymbols(const std::vector<std::string>& symbols) {
        std::string out;
        for (const auto& s : symbols) {
            if (!out.empty()) out += ',';
            out += s;
        }
        return out;
    }
}

MarketDataCoreEngine::MarketDataCoreEngine(Authenticator& auth, const ServerMdcConfig& config)
    : m_auth(auth)
    , m_host(config.host)
    , m_port(config.port)
    , m_target(config.target)
    , m_useJwt(config.useJwt)
    , m_sslCaBundle(config.sslCaBundle)
{
    const char* defaultCaBundle = "resources/certs/ca-bundle.crt";
    const std::string bundlePath = !m_sslCaBundle.empty() ? m_sslCaBundle : defaultCaBundle;
    try {
        sLog_Data("Using CA bundle: path=" << bundlePath);
        m_sslCtx.load_verify_file(bundlePath);
    } catch (const std::exception& e) {
        sLog_Error("Failed to load CA bundle, falling back to system paths: path=" << bundlePath
                   << " error=" << e.what());
        m_sslCtx.set_default_verify_paths();
    }
    m_sslCtx.set_verify_mode(ssl::verify_peer);

    sLog_App("MarketDataCore initialized: host=" << m_host << " port=" << m_port
             << " target=" << m_target << " jwt=" << m_useJwt);
    m_transport = std::make_unique<BeastWsTransport>(m_ioc, m_sslCtx);
    m_transport->onStatus([this](bool up){
        if (m_ingestObserver) observeIngest(up ? IngestKind::TransportUp : IngestKind::TransportDown);
        m_connected.store(up);
        sLog_Data("WebSocket transport status changed: " << (up ? "UP" : "DOWN")
                  << " host=" << m_host);
        if (up) {
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
        } else {
            emitError("Transport down");
        }
    });
    m_transport->onError([this](std::string err){ emitError(std::move(err)); });
    m_transport->onMessage([this](std::string payload){
        if (m_ingestObserver) observeIngest(IngestKind::Frame, payload);
        try {
            auto j = nlohmann::json::parse(payload);
            dispatch(j);
        } catch (const nlohmann::json::parse_error& e) {
            sLog_Error("JSON parse error in transport message: bytes=" << payload.size()
                       << " head=" << payload.substr(0, 200) << " error=" << e.what());
        } catch (const std::exception& e) {
            sLog_Error("Error processing transport message: bytes=" << payload.size()
                       << " head=" << payload.substr(0, 200) << " error=" << e.what());
        }
    });
}

MarketDataCoreEngine::~MarketDataCoreEngine() {
    stop();
}

void MarketDataCoreEngine::observeIngest(IngestKind kind, std::string_view payload,
                                         std::string_view product, std::string_view reason) noexcept {
    const auto systemNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto steadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    try {
        m_ingestObserver({kind, systemNs, steadyNs, payload, product, reason});
    } catch (const std::exception& e) {
        sLog_Error("Ingest observer exception: " << e.what());
    } catch (...) {
        sLog_Error("Ingest observer exception (unknown)");
    }
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
    } else {
        emitBookInvalidated(std::string(), "disconnected");
    }
    m_lastSequenceNum = -1;
    if (m_onConnectionStatus) {
        try {
            m_onConnectionStatus(connected);
        } catch (const std::exception& e) {
            sLog_Error(std::string("Connection status callback exception: ") + e.what());
        }
    }
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
        sLog_Data("subscribeToSymbols: new=" << joinSymbols(new_symbols)
                  << " totalProducts=" << m_products.size());
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
        sLog_Data("unsubscribeFromSymbols: removed=" << joinSymbols(removed_symbols)
                  << " totalProducts=" << m_products.size());
        sendSubscriptionMessage("unsubscribe", removed_symbols);
    }
}

void MarketDataCoreEngine::start() {
    if (!m_running.exchange(true)) {
        sLog_App("Starting MarketDataCore: host=" << m_host << " port=" << m_port
                 << " target=" << m_target);
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
    sentinel::logging::setCurrentThreadName("mdc-io");
    // io_context::run() can exit on unhandled handler exception; loop keeps I/O thread alive.
    while (m_running.load()) {
        try {
            m_ioc.run();
            if (m_running.load()) {
                m_ioc.restart();
            }
        } catch (const std::exception& e) {
            sLog_Error("IO context thread exception, restarting I/O loop: error=" << e.what());
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
    
    sLog_Data("Scheduling reconnect: delayMs="
              << std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()
              << " backoffSec=" << m_backoffDuration.count()
              << " host=" << m_host);
    m_reconnectTimer.expires_after(delay);
    m_reconnectTimer.async_wait([this](beast::error_code ec) {
        if (ec || !m_running) return;
        
        sLog_Data("Attempting reconnection: host=" << m_host << " port=" << m_port);
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
            // Routine before the first connect: the request replays on connect.
            sLog_Data("Transport not connected, staging " << type << " for replay on connect: symbols="
                      << joinSymbols(symbolsCopy));
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
                sLog_Error("JWT creation failed in subscription handler: type=" << type
                           << " symbols=" << joinSymbols(symbolsCopy) << " error=" << e.what());
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
                    sLog_Data("WS " << type << " frame: " << j.dump());
                } catch (const std::exception&) {
                    sLog_Data("WS " << type << " frame (raw): " << frame);
                }
                m_transport->send(frame);
            }
        }
    });
}

void MarketDataCoreEngine::emitBookInvalidated(const std::string& productId, const std::string& reason) {
    if (m_ingestObserver) observeIngest(IngestKind::BookInvalidated, {}, productId, reason);
    sLog_Warning("Order book invalidated: product=" << (productId.empty() ? std::string("*") : productId)
                 << " reason=" << reason);
    if (m_onLiveOrderBookInvalidated) {
        try {
            m_onLiveOrderBookInvalidated(productId, reason);
        } catch (const std::exception& e) {
            sLog_Error("Order book invalidated callback exception: " << e.what());
        }
    }
}

void MarketDataCoreEngine::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;
    auto arrival_time = std::chrono::system_clock::now();

    // A missing sequence number means a dropped message on this connection: every
    // book is now unknown. Reconnecting resubscribes, which brings fresh snapshots.
    if (message.contains("sequence_num") && message["sequence_num"].is_number_integer()) {
        const int64_t seq = message["sequence_num"].get<int64_t>();
        if (m_lastSequenceNum >= 0 && seq != m_lastSequenceNum + 1) {
            emitBookInvalidated(std::string(), "sequence gap expected=" + std::to_string(m_lastSequenceNum + 1)
                                                   + " got=" + std::to_string(seq));
            m_lastSequenceNum = -1;
            triggerImmediateReconnect("sequence gap");
            return;
        }
        m_lastSequenceNum = seq;
    }
    
    sLog_Probe("ws.rx", message.dump());

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
                        sLog_Data("Subscription confirmed: symbols=" << joinSymbols(ev.productIds));
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
            sLog_Warning("sequence_num parse issue: value=" << message["sequence_num"].dump()
                         << " error=" << e.what());
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

namespace {
// One L2 level: side, positive finite price, finite non-negative quantity (0 = remove).
bool parseLevel(const nlohmann::json& update, bool& isBid, double& price, double& quantity) {
    if (!update.is_object()) return false;
    const auto side = update.find("side");
    const auto priceIt = update.find("price_level");
    const auto qtyIt = update.find("new_quantity");
    if (side == update.end() || priceIt == update.end() || qtyIt == update.end() ||
        !side->is_string() || !priceIt->is_string() || !qtyIt->is_string()) {
        return false;
    }
    const std::string normalized = side_norm::normalize(side->get<std::string>());
    if (normalized != "bid" && normalized != "ask") return false;
    isBid = (normalized == "bid");
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    price = Cpp20Utils::fastStringToDouble(priceIt->get_ref<const std::string&>(), kNaN);
    quantity = Cpp20Utils::fastStringToDouble(qtyIt->get_ref<const std::string&>(), kNaN);
    return std::isfinite(price) && price > 0.0 && std::isfinite(quantity) && quantity >= 0.0;
}
} // namespace

void MarketDataCoreEngine::handleOrderBookSnapshot(const nlohmann::json& event,
                                                   const std::string& product_id,
                                                   const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
    std::vector<OrderBookLevel> sparse_bids;
    std::vector<OrderBookLevel> sparse_asks;
    int malformed = 0;
    for (const auto& update : event["updates"]) {
        bool isBid = false;
        double price = 0.0;
        double quantity = 0.0;
        if (!parseLevel(update, isBid, price, quantity)) {
            ++malformed;
            continue;
        }
        if (quantity > 0.0) {
            (isBid ? sparse_bids : sparse_asks).push_back(OrderBookLevel{price, quantity});
        }
    }
    if (malformed > 0) {
        emitBookInvalidated(product_id, "malformed snapshot entries=" + std::to_string(malformed));
        triggerImmediateReconnect("malformed l2");
        return;
    }
    const int64_t envelopeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        exchange_timestamp.time_since_epoch()).count();
    if (m_onLiveOrderBookInitialized) {
        try {
            m_onLiveOrderBookInitialized(product_id, sparse_bids, sparse_asks, envelopeMs);
        } catch (const std::exception& e) {
            sLog_Error("Order book init callback exception: product=" << product_id
                       << " error=" << e.what());
        }
    }
}

void MarketDataCoreEngine::handleOrderBookUpdate(const nlohmann::json& event,
                                                 const std::string& product_id,
                                                 const std::chrono::system_clock::time_point& exchange_timestamp) {
    if (!event.contains("updates") || product_id.empty()) return;
    // thread_local is safe here because dispatch() is always called from the same io_context thread.
    // Avoids repeated allocations in hot path. If io_context ever uses a thread pool, this must be revisited.
    thread_local std::vector<BookLevelUpdate> levelUpdates;
    levelUpdates.clear();
    levelUpdates.reserve(event["updates"].size());

    for (const auto& update : event["updates"]) {
        bool isBid = false;
        double price = 0.0;
        double quantity = 0.0;
        if (!parseLevel(update, isBid, price, quantity)) {
            // A level we cannot apply leaves the book in an unknown state.
            emitBookInvalidated(product_id, "malformed update level");
            triggerImmediateReconnect("malformed l2");
            return;
        }
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
                sLog_Error("Order book level updates callback exception: product=" << product_id
                           << " updates=" << updatesPayload.size() << " error=" << e.what());
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
                sLog_Warning("Heartbeat stale, reconnecting: silenceMs=" << (nowMs - lastMs)
                             << " thresholdMs=" << kHeartbeatStaleThresholdMs
                             << " host=" << m_host);
                triggerImmediateReconnect("stale heartbeat");
                return;
            }
            startHeartbeatWatchdog();
        });
    });
}

void MarketDataCoreEngine::triggerImmediateReconnect(const char* reason) {
    net::post(m_strand, [this, r = std::string(reason)](){
        if (m_ingestObserver) observeIngest(IngestKind::ResyncRequested, {}, {}, r);
        sLog_Data("Immediate reconnect: reason=" << r);
        // Use 5s backoff for stale heartbeat to avoid hammering Coinbase when they're slow.
        m_backoffDuration = (r == "stale heartbeat")
            ? std::chrono::seconds(5)
            : std::chrono::seconds(1);
        m_reconnectTimer.cancel();
        if (m_transport) {
            m_transport->close();
            scheduleReconnect();
        }
    });
}
