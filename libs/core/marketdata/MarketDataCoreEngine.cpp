// MarketDataCoreEngine: WebSocket market data; I/O on worker thread. Coinbase-like API.
#include "MarketDataCoreEngine.hpp"
#include "SentinelLogging.hpp"
#include "dispatch/MessageDispatcher.hpp"
#include "dispatch/BookParser.hpp"
#include "dispatch/Channels.hpp"
#include "Cpp20Utils.hpp"
#include <cmath>
#include <limits>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <string>

namespace {
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

MarketDataCoreEngine::MarketDataCoreEngine(Authenticator& auth, const ServerMdcConfig& config,
    std::string product, net::io_context& io, ssl::context& tls, TransportFactory factory,
    ReconnectPolicy policy, Clock clock, ConnectPermit permit, ConnectPermit subscribePermit, std::function<void()> cancelPermits, Jitter jitter)
    : m_host(config.host), m_port(config.port), m_target(config.target), m_useJwt(config.useJwt),
      m_product(std::move(product)), m_auth(auth), m_strand(io.get_executor()),
      m_reconnectPolicy(policy), m_clock(std::move(clock)), m_connectPermit(std::move(permit)), m_subscribePermit(std::move(subscribePermit)),
      m_cancelPermits(std::move(cancelPermits)), m_jitter(std::move(jitter)), m_backoffDuration(policy.initialDelay) {
    if (m_product.empty() || policy.initialDelay.count() <= 0 || policy.maximumDelay < policy.initialDelay ||
        policy.watchdogInterval.count() <= 0 || policy.heartbeatStale < policy.watchdogInterval ||
        policy.level2Stale < policy.watchdogInterval || policy.level2RetryMaximum < policy.level2Stale ||
        policy.staleHeartbeatDelay < policy.initialDelay || policy.staleHeartbeatDelay > policy.maximumDelay ||
        config.connectTimeoutMs <= 0 || config.closeTimeoutMs <= 0)
        throw std::invalid_argument("invalid market-data product/timing policy");
    m_liveness.retryMs = policy.level2Stale.count();
    if (factory) m_transport = factory(m_product, io, tls);
    else {
        BeastWsTransport::Options options;
        options.product = m_product;
        options.connectTimeout = std::chrono::milliseconds(config.connectTimeoutMs);
        options.closeTimeout = std::chrono::milliseconds(config.closeTimeoutMs);
        m_transport = std::make_unique<BeastWsTransport>(io, tls, std::move(options));
    }
    if (!m_transport) throw std::invalid_argument("market-data transport factory returned null");
}

MarketDataCoreEngine::~MarketDataCoreEngine() = default;

void MarketDataCoreEngine::start() {
    auto weak = weak_from_this();
    m_transport->onStatus([weak](bool up) {
        if (auto self = weak.lock()) net::dispatch(self->m_strand, [self, up] { self->transportStatus(up); });
    });
    m_transport->onError([weak](std::string error) {
        if (auto self = weak.lock()) net::dispatch(self->m_strand,
            [self, error = std::move(error)] { self->emitError(error); });
    });
    m_transport->onMessage([weak](std::string payload) {
        if (auto self = weak.lock()) net::dispatch(self->m_strand,
            [self, payload = std::move(payload)]() mutable { self->receive(std::move(payload)); });
    });
    net::dispatch(m_strand, [self = shared_from_this()] {
        self->m_running = true;
        self->m_watchdogAt = self->m_clock();
        self->m_downSince = self->m_clock();
        self->m_nextDownAlarm = self->m_downSince + 120'000'000;
        self->scheduleReconnect(true);
    });
}

void MarketDataCoreEngine::stop(std::function<void()> completion) {
    net::dispatch(m_strand, [self = shared_from_this(), completion = std::move(completion)]() mutable {
        self->m_running = false;
        self->m_cancelPermits();
        self->m_reconnectScheduled = false;
        self->m_transport->retire();
        self->m_stopCompletion = std::move(completion);
        if (!self->m_closePending) {
            self->m_closePending = true;
            self->m_transport->close();
        }
    });
}

void MarketDataCoreEngine::transportStatus(bool up) {
    if (!up) m_cancelPermits();
    if (up) ++m_connection;
    if (up) m_downSince = -1;
    else if (m_downSince < 0) {
        m_downSince = m_clock();
        m_nextDownAlarm = m_downSince + 120'000'000;
    }
    m_subscriptionsPending = false;
    if (m_ingestObserver) observeIngest(up ? IngestKind::TransportUp : IngestKind::TransportDown);
    m_closePending = false;
    m_connected = up && m_running;
    if (!m_running) {
        if (!up && m_stopCompletion) {
            auto done = std::exchange(m_stopCompletion, {});
            done();
        }
        return;
    }
    sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Transport status=" << (up ? "UP" : "DOWN"));
    if (up) {
        m_reconnectScheduled = false;
        m_lastHeartbeatMs = m_clock() / 1000;
        m_liveness.lastLevel2Ms = m_lastHeartbeatMs;
        m_liveness.snapshotAccepted = false;
        emitConnectionStatus(true);
        m_subscriptionsPending = true;
        if (m_subscribePermit(m_clock())) { m_subscriptionsPending = false; sendSubscriptions(); }
    } else {
        emitConnectionStatus(false);
        scheduleReconnect();
    }
}

void MarketDataCoreEngine::receive(std::string payload) {
    if (m_ingestObserver) observeIngest(IngestKind::Frame, payload);
    if (!m_running || !m_connected || m_closePending) return;
    m_lastHeartbeatMs = m_clock() / 1000;
    try {
        dispatch(nlohmann::json::parse(payload));
    } catch (const std::exception& e) {
        emitError(std::string("Malformed message: ") + e.what());
        reconnectNow("malformed message");
    }
}

void MarketDataCoreEngine::tick() {
    net::dispatch(m_strand, [self = shared_from_this()] {
        auto& e = *self;
        if (!e.m_running) return;
        const auto now = e.m_clock();
        if (e.m_reconnectScheduled && now >= e.m_connectAt && e.m_connectPermit(now)) {
            e.m_reconnectScheduled = false;
            ++e.m_attempt;
            sLog_Probe("feeds.connect", "product=" << e.m_product << " conn=" << e.m_connection << " attempt=" << e.m_attempt
                       << " bucketWaitUs=" << now - e.m_connectAt);
            e.m_transport->connect(e.m_host, e.m_port, e.m_target);
        }
        if (e.m_connected && !e.m_closePending && e.m_subscriptionsPending && e.m_subscribePermit(now)) {
            e.m_subscriptionsPending = false;
            e.sendSubscriptions();
        }
        if (e.m_downSince >= 0 && now >= e.m_nextDownAlarm) {
            sLog_Error("product=" << e.m_product << " conn=" << e.m_connection << " attempt=" << e.m_attempt
                       << " Feed down: downMs=" << (now - e.m_downSince) / 1000);
            e.m_nextDownAlarm = now + 60'000'000;
        }
        if (now < e.m_watchdogAt) return;
        e.m_watchdogAt = now + e.m_reconnectPolicy.watchdogInterval.count() * 1000;
        if (!e.m_connected || e.m_closePending) return;
        if (now / 1000 - e.m_lastHeartbeatMs >= e.m_reconnectPolicy.heartbeatStale.count())
            e.reconnectNow("stale heartbeat");
        else e.checkLevel2Silence(now / 1000);
    });
}

MarketDataCoreEngine::Stats MarketDataCoreEngine::stats() const {
    const auto now = m_clock() / 1000;
    return {m_product, m_connected, m_connection, m_connection ? m_connection - 1 : 0,
            m_lastSequenceNum, m_lastHeartbeatMs < 0 ? -1 : now - m_lastHeartbeatMs,
            m_connection ? now - m_liveness.lastLevel2Ms : -1};
}

void MarketDataCoreEngine::observeIngest(IngestKind kind, std::string_view payload,
                                         std::string_view reason) noexcept {
    const auto systemNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto steadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    try {
        m_ingestObserver({kind, m_connection, systemNs, steadyNs, payload, m_product, reason});
    } catch (const std::exception& e) {
        sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Ingest observer exception: " << e.what());
    } catch (...) {
        sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Ingest observer exception (unknown)");
    }
}

inline void MarketDataCoreEngine::emitError(std::string msg) {
    if (!m_running) {
        // A stopped (removed or shutting down) engine's socket is still closing.
        // Its errors must not read as the live product: after remove + re-add a
        // new engine owns this product name. No callback, no product= key.
        sLog_Data("Retired feed socket error ignored: retiredProduct=" << m_product << " conn=" << m_connection
                  << " attempt=" << m_attempt << " error=" << msg);
        return;
    }
    sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " error=" << msg);
    if (m_onError) {
        try {
            m_onError(m_product, msg);
        } catch (const std::exception& e) {
            sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " " << std::string("Error callback exception: ") + e.what());
        }
    }
}

inline void MarketDataCoreEngine::emitConnectionStatus(bool connected) {
    if (connected) {
        m_loggedEmptySubscriptionAck = m_warnedMissingLevel2 = false;
    } else {
        emitBookInvalidated("disconnected");
    }
    m_lastSequenceNum = -1;
    if (m_onConnectionStatus) {
        try {
            m_onConnectionStatus(m_product, connected);
        } catch (const std::exception& e) {
            sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " " << std::string("Connection status callback exception: ") + e.what());
        }
    }
}

void MarketDataCoreEngine::scheduleReconnect(bool initial) {
    if (!m_running || m_connected || m_reconnectScheduled || m_closePending) return;
    m_reconnectScheduled = true;
    const auto delay = initial ? std::chrono::milliseconds(0) : m_backoffDuration;
    if (!initial) m_backoffDuration = std::min(m_backoffDuration * 2, m_reconnectPolicy.maximumDelay);
    const auto jitter = std::clamp(m_jitter(), std::chrono::milliseconds(0), std::chrono::milliseconds(1000));
    m_connectAt = m_clock() + (delay + jitter).count() * 1000;
    sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Scheduling connect: backoffMs=" << delay.count() << " jitterMs=" << jitter.count());
}

void MarketDataCoreEngine::sendSubscriptions() {
    std::string jwt;
    try {
        if (m_useJwt) jwt = m_auth.createJwt();
    } catch (const std::exception& e) {
        emitError(std::string("JWT creation failed: ") + e.what());
        reconnectNow("authentication failure");
        return;
    }
    for (const auto& frame : SubscriptionManager::buildSubscribeMsgs(m_product, jwt))
        m_transport->send(frame);
}

void MarketDataCoreEngine::emitBookInvalidated(const std::string& reason) {
    m_liveness.snapshotAccepted = false;
    if (m_ingestObserver) observeIngest(IngestKind::BookInvalidated, {}, reason);
    sLog_Warning("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Order book invalidated: reason=" << reason);
    if (m_onLiveOrderBookInvalidated) {
        try {
            m_onLiveOrderBookInvalidated(m_product, reason);
        } catch (const std::exception& e) {
            sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Order book invalidated callback exception: " << e.what());
        }
    }
}

void MarketDataCoreEngine::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) throw std::runtime_error("expected object");
    auto arrival_time = std::chrono::system_clock::now();

    // A discontinuity invalidates only this connection's product.
    if (message.contains("sequence_num") && message["sequence_num"].is_number_integer()) {
        const int64_t seq = message["sequence_num"].get<int64_t>();
        if (seq < 0 || seq == std::numeric_limits<int64_t>::max()) throw std::runtime_error("invalid sequence number");
        if (m_lastSequenceNum >= 0 && seq != m_lastSequenceNum + 1) {
            reconnectNow("sequence gap expected=" + std::to_string(m_lastSequenceNum + 1) + " got=" + std::to_string(seq));
            return;
        }
        m_lastSequenceNum = seq;
    }
    
    sLog_Probe("ws.rx", "product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " " << message.dump());

    std::string channel = message.value("channel", "");
    m_lastHeartbeatMs = m_clock() / 1000;
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
                    sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Provider error: " << ev.message << " | raw=" << message.dump());
                    emitError(ev.message);
                    reconnectNow("provider error");
                } else if constexpr (std::is_same_v<T, SubscriptionAckEvent>) {
                    if (ev.level2ProductIds) {
                        const auto& ids = *ev.level2ProductIds;
                        if (!m_warnedMissingLevel2 && std::find(ids.begin(), ids.end(), m_product) == ids.end()) {
                            m_warnedMissingLevel2 = true;
                            sLog_Warning("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Subscription ack missing level2 product; acknowledged=" << joinSymbols(ids));
                        }
                    }
                    if (!ev.productIds.empty()) {
                        sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Subscription confirmed: symbols=" << joinSymbols(ev.productIds));
                    } else if (!m_loggedEmptySubscriptionAck) {
                        m_loggedEmptySubscriptionAck = true;
                        sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Subscription confirmed with empty product list; raw payload: "
                                  << message.dump());
                    }
                }
            }, evt);
        }
        if (channel.empty() && result.events.empty()) {
            sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " MDC unclassified message: " << message.dump());
        }
    }
    
    if (!m_connected) return;
    if (channel == ch::kTrades) {
        handleMarketTrades(message, arrival_time);
    } else if (channel == ch::kL2Data) {
        handleOrderBookData(message, arrival_time);
    }
}

void MarketDataCoreEngine::handleMarketTrades(const nlohmann::json& message,
                                              const std::chrono::system_clock::time_point& arrival_time) {
    if (!message.contains("events") || !message["events"].is_array()) throw std::runtime_error("missing events");
    
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
        if (trade.product_id != m_product) throw std::runtime_error("unexpected trade product");
        
        if (m_onTrade) {
            try {
                m_onTrade(trade);
            } catch (const std::exception& e) {
                sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " " << std::string("Trade callback exception: ") + e.what());
            }
        }
        
    }
}

Trade MarketDataCoreEngine::createTradeFromJson(const nlohmann::json& trade_data,
                                                const std::chrono::system_clock::time_point& arrival_time) {
    Trade trade = MessageDispatcher::parseTrade(trade_data, arrival_time);
    // Coinbase market_trades reports the resting maker. All server consumers
    // receive the initiating aggressor side from this boundary onward.
    if (trade.side == AggressorSide::Buy) trade.side = AggressorSide::Sell;
    else if (trade.side == AggressorSide::Sell) trade.side = AggressorSide::Buy;
    return trade;
}

void MarketDataCoreEngine::handleOrderBookData(const nlohmann::json& message,
                                               const std::chrono::system_clock::time_point& arrival_time) {
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
    
    if (!message.contains("events") || !message["events"].is_array()) throw std::runtime_error("missing events");
    
    for (const auto& event : message["events"]) {
        std::string eventType = event.value("type", "");
        std::string product_id = event.value("product_id", "");
        if (eventType == "snapshot" || eventType == "update") {
            if (product_id != m_product) throw std::runtime_error("unexpected L2 product");
        }
        if (eventType == "snapshot") {
            handleOrderBookSnapshot(event, product_id, exchange_timestamp);
        } else if (eventType == "update") {
            handleOrderBookUpdate(event, product_id, exchange_timestamp);
        }
        if (!m_connected) return;
    }
}

void MarketDataCoreEngine::handleOrderBookSnapshot(const nlohmann::json& event,
                                                   const std::string& product_id,
                                                   const std::chrono::system_clock::time_point& exchange_timestamp) {
    std::vector<OrderBookLevel> sparse_bids, sparse_asks;
    const int malformed = sentinel::dispatch::parseSnapshot(event, sparse_bids, sparse_asks);
    if (malformed > 0) {
        reconnectNow("malformed snapshot entries=" + std::to_string(malformed));
        return;
    }
    m_liveness.snapshotAccepted = true;
    m_backoffDuration = m_reconnectPolicy.initialDelay;
    m_liveness.lastLevel2Ms = m_clock() / 1000;
    const int64_t envelopeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        exchange_timestamp.time_since_epoch()).count();
    if (m_onLiveOrderBookInitialized) {
        try {
            m_onLiveOrderBookInitialized(product_id, sparse_bids, sparse_asks, envelopeMs);
        } catch (const std::exception& e) {
            sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Order book init callback exception: product=" << product_id
                       << " error=" << e.what());
        }
    }
}

void MarketDataCoreEngine::handleOrderBookUpdate(const nlohmann::json& event,
                                                 const std::string& product_id,
                                                 const std::chrono::system_clock::time_point& exchange_timestamp) {
    auto& levelUpdates = m_levelUpdates;
    if (!sentinel::dispatch::parseUpdates(event, levelUpdates)) {
        reconnectNow("malformed update level");
        return;
    }

    if (!levelUpdates.empty()) {
        if (!m_liveness.snapshotAccepted) return;
        m_liveness.lastLevel2Ms = m_clock() / 1000;
        m_liveness.retryMs = m_reconnectPolicy.level2Stale.count();
        const int64_t exchangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            exchange_timestamp.time_since_epoch()).count();
        const auto& updatesPayload = levelUpdates;
        if (m_onLiveOrderBookLevelUpdates) {
            try {
                m_onLiveOrderBookLevelUpdates(product_id, updatesPayload, exchangeMs);
            } catch (const std::exception& e) {
                sLog_Error("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Order book level updates callback exception: product=" << product_id
                           << " updates=" << updatesPayload.size() << " error=" << e.what());
            }
        }
    }
}

void MarketDataCoreEngine::handleHeartbeats(const nlohmann::json&) {
    m_lastHeartbeatMs = m_clock() / 1000;
}

void MarketDataCoreEngine::checkLevel2Silence(int64_t nowMs) {
    if (nowMs - m_liveness.lastLevel2Ms < m_liveness.retryMs) return;
    sLog_Warning("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Level2 silent: silenceMs=" << nowMs - m_liveness.lastLevel2Ms
                 << " retryMs=" << m_liveness.retryMs);
    m_liveness.retryMs = std::min(m_liveness.retryMs * 2, m_reconnectPolicy.level2RetryMaximum.count());
    reconnectNow("level2 silent");
}

void MarketDataCoreEngine::requestResnapshot() {
    net::dispatch(m_strand, [self = shared_from_this()] {
        auto& e = *self;
        if (!e.m_running || !e.m_connected || e.m_closePending || e.m_reconnectScheduled) return;
        const auto now = e.m_clock() / 1000;
        if (e.m_lastResnapshotMs >= 0 && now - e.m_lastResnapshotMs < e.m_reconnectPolicy.resnapshotCooldown.count()) return;
        e.m_lastResnapshotMs = now;
        e.reconnectNow("consumer resnapshot");
    });
}

void MarketDataCoreEngine::reconnectNow(const std::string& reason) {
    if (!m_running || !m_connected || m_closePending || m_reconnectScheduled) return;
    emitBookInvalidated(reason);
    if (m_ingestObserver) observeIngest(IngestKind::ResyncRequested, {}, reason);
    sLog_Data("product=" << m_product << " conn=" << m_connection << " attempt=" << m_attempt << " Reconnecting: reason=" << reason);
    if (reason == "stale heartbeat")
        m_backoffDuration = std::max(m_backoffDuration, m_reconnectPolicy.staleHeartbeatDelay);
    // A subscribe batch still waiting for the process bucket must leave the
    // queue now: its ticket would otherwise hold every other product's
    // subscribe until this socket's close completes (Coinbase drops a socket
    // that has not subscribed within 5 s).
    m_subscriptionsPending = false;
    m_cancelPermits();
    m_closePending = true;
    m_connected = false;
    m_transport->close();
}
