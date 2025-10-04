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
#include "Cpp20Utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <QString>
#include <QMetaObject>
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
    // Configure SSL context
    m_sslCtx.set_default_verify_paths();
    m_sslCtx.set_verify_mode(ssl::verify_peer);
    
    sLog_App("🏗️ MarketDataCore initialized");
}

MarketDataCore::~MarketDataCore() {
    stop();
    sLog_App("🏗️ MarketDataCore destroyed");
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
        sendSubscriptionMessage("unsubscribe", removed_symbols);
    }
}

void MarketDataCore::start() {
    if (!m_running.exchange(true)) {
        sLog_App("🚀 Starting MarketDataCore...");
        
        // Reset backoff on fresh start
        m_backoffDuration = std::chrono::seconds(1);
        
        // Create work guard to keep io_context alive
        m_workGuard.emplace(m_ioc.get_executor());
        
        // Restart io_context in case it was previously stopped
        m_ioc.restart();
        
        // Start I/O thread
        m_ioThread = std::thread(&MarketDataCore::run, this);
    }
}

void MarketDataCore::stop() {
    if (m_running.exchange(false)) {
        sLog_App("🛑 Stopping MarketDataCore...");
        
        // Cancel reconnect timer
        m_reconnectTimer.cancel();
        
        // Post close operation to strand for thread safety
        net::post(m_strand, [this]() {
            beast::error_code ec;
            
            // Cancel any pending timer operations
            m_reconnectTimer.cancel();
            
            // Close WebSocket gracefully if open
            if (m_ws.is_open()) {
                m_ws.async_close(websocket::close_code::normal,
                    [this](beast::error_code) {
                        // Release work guard to allow io_context to exit
                        m_workGuard.reset();
                    });
            } else {
                // Release work guard immediately if WS not open
                m_workGuard.reset();
            }
        });
        
        // Join thread
        if (m_ioThread.joinable()) {
            m_ioThread.join();
        }
        
        sLog_App("✅ MarketDataCore stopped");
    }
}

void MarketDataCore::run() {
    sLog_Data(QString("🔌 Starting connection to %1:%2").arg(QString::fromStdString(m_host)).arg(QString::fromStdString(m_port)));
    
    // Start the connection process
    m_resolver.async_resolve(m_host, m_port,
        beast::bind_front_handler(&MarketDataCore::onResolve, this));
    
    // Run the IO context - this blocks until stopped
    m_ioc.run();
    
    sLog_Data("🔌 IO context stopped");
}

void MarketDataCore::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "❌ Resolve error: " << ec.message() << std::endl;
        emitError(QString("Resolve error: %1").arg(QString::fromStdString(ec.message())));
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🔍 DNS resolved, connecting...");
    
    // Set connection timeout
    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    
    // Connect to the endpoint
    get_lowest_layer(m_ws).async_connect(results,
        beast::bind_front_handler(&MarketDataCore::onConnect, this));
}

void MarketDataCore::onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
        std::cerr << "❌ Connect error: " << ec.message() << std::endl;
        emitError(QString("Connect error: %1").arg(QString::fromStdString(ec.message())));
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🔗 TCP connected, starting SSL handshake...");
    
    // Set SNI hostname for SSL
    if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), m_host.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "❌ SSL SNI error: " << ssl_ec.message() << std::endl;
        emitError(QString("SSL SNI error: %1").arg(QString::fromStdString(ssl_ec.message())));
        scheduleReconnect();
        return;
    }
    
    // Set hostname verification for SSL
    SSL* ssl_handle = m_ws.next_layer().native_handle();
    if (SSL_set1_host(ssl_handle, m_host.c_str()) != 1) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "❌ SSL hostname verification setup error: " << ssl_ec.message() << std::endl;
        emitError(QString("SSL hostname verification setup error: %1").arg(QString::fromStdString(ssl_ec.message())));
        scheduleReconnect();
        return;
    }
    
    // Ensure proper certificate verification
    m_ws.next_layer().set_verify_mode(ssl::verify_peer);
    
    // Set SSL handshake timeout
    get_lowest_layer(m_ws).expires_after(std::chrono::seconds(30));
    
    // Perform SSL handshake
    m_ws.next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&MarketDataCore::onSslHandshake, this));
}

void MarketDataCore::onSslHandshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "❌ SSL handshake error: " << ec.message() << std::endl;
        emitError(QString("SSL handshake error: %1").arg(QString::fromStdString(ec.message())));
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🔐 SSL handshake complete, starting WebSocket handshake...");
    
    // Disable timeout (WebSocket has its own timeout settings)
    get_lowest_layer(m_ws).expires_never();
    
    // Set WebSocket timeout options
    m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    
    // Perform WebSocket handshake
    m_ws.async_handshake(m_host, m_target,
        beast::bind_front_handler(&MarketDataCore::onWsHandshake, this));
}

void MarketDataCore::onWsHandshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "❌ WebSocket handshake error: " << ec.message() << std::endl;
        emitError(QString("WebSocket handshake error: %1").arg(QString::fromStdString(ec.message())));
        
        // 🚨 FIX: Emit disconnected status on handshake failure
        {
            QPointer<MarketDataCore> self(this);
            QMetaObject::invokeMethod(this, [self]() {
                if (!self) return;
                emit self->connectionStatusChanged(false);
            }, Qt::QueuedConnection);
        }
        
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🌐 WebSocket connected! Waiting for subscription requests...");
    
    // 🚨 FIX: Emit connected status when WebSocket is ready
    // Reset backoff on successful WS handshake
    m_backoffDuration = std::chrono::seconds(1);
    sLog_Data("🔁 Backoff reset to 1s after successful WS handshake");

    {
        QPointer<MarketDataCore> self(this);
        QMetaObject::invokeMethod(this, [self]() {
            if (!self) return;
            emit self->connectionStatusChanged(true);
        }, Qt::QueuedConnection);
    }

    // Replay desired subscriptions and start heartbeat
    replaySubscriptionsOnConnect();
    startHeartbeat();
    
    // Connection is live, start reading messages from the server.
    // Subscriptions will be sent on-demand via the public methods.
    m_ws.async_read(m_buf,
        beast::bind_front_handler(&MarketDataCore::onRead, this));
}

void MarketDataCore::onWrite(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "❌ Write error: " << ec.message() << std::endl;
        emitError(QString("Write error: %1").arg(QString::fromStdString(ec.message())));
        scheduleReconnect();
        return;
    }
    
    // This callback is now primarily for confirming subscription messages
    sLog_Data("✅ Subscription message sent!");
}

void MarketDataCore::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "❌ Read error: " << ec.message() << std::endl;
        emitError(QString("Read error: %1").arg(QString::fromStdString(ec.message())));
        scheduleReconnect();
        return;
    }
    
    // Convert buffer to string
    std::string payload = beast::buffers_to_string(m_buf.data());
    m_buf.consume(m_buf.size());
    
    // Parse and dispatch the message
    try {
        auto message = nlohmann::json::parse(payload);
        dispatch(message);
    } catch (const std::exception& e) {
        std::cerr << "❌ JSON parse error: " << e.what() << std::endl;
        // Continue reading even if one message fails
    }
    
    // Continue reading the next message
    if (m_running) {
        m_ws.async_read(m_buf,
            beast::bind_front_handler(&MarketDataCore::onRead, this));
    }
}

void MarketDataCore::doClose() {
    if (m_ws.is_open()) {
        sLog_Data("🔌 Closing WebSocket connection...");
        m_ws.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&MarketDataCore::onClose, this));
    }
}

void MarketDataCore::onClose(beast::error_code ec) {
    if (ec) {
        std::cerr << "❌ Close error: " << ec.message() << std::endl;
        emitError(QString("Close error: %1").arg(QString::fromStdString(ec.message())));
        
        // 🚀 PHASE 2.1: Record network error
        if (m_monitor) {
            m_monitor->recordNetworkError(QString("Close error: %1").arg(QString::fromStdString(ec.message())));
        }
    } else {
        sLog_Data("✅ WebSocket connection closed");
    }
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
    
    sLog_Data(QString("🔄 Scheduling reconnect in %1ms (backoff: %2s)...")
        .arg(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count())
        .arg(m_backoffDuration.count()));
    
    // Reset the stream state and clear any queued writes
    if (m_ws.is_open()) {
        doClose();
    }
    clearWriteQueue();
    
    // NON-BLOCKING timer-based reconnect
    m_reconnectTimer.expires_after(delay);
    m_reconnectTimer.async_wait([this](beast::error_code ec) {
        if (ec || !m_running) return;
        
        sLog_Data("🔄 Attempting reconnection...");
        
        // 🚀 PHASE 2.1: Record network reconnection
        if (m_monitor) {
            m_monitor->recordNetworkReconnect();
        }
        
        m_resolver.async_resolve(m_host, m_port,
            beast::bind_front_handler(&MarketDataCore::onResolve, this));
    });
}

void MarketDataCore::clearWriteQueue() {
    // Strand context only
    m_writeQueue.clear();
    m_writeInProgress = false;
}

void MarketDataCore::sendSubscriptionMessage(const std::string& type, const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        return;
    }

    // Post to the strand to ensure thread-safe access to the WebSocket stream
    net::post(m_strand, [this, type, symbols]() {
        if (!m_ws.is_open()) {
            sLog_Warning("WebSocket not open, staging subscription request for replay on connect.");
            // Stage by updating desired product set; replay on handshake
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

        // Helper to build and send a message for a specific channel
        auto sendMessage = [this](const std::string& channel, const std::string& type, const std::vector<std::string>& symbols) {
            nlohmann::json msg;
            msg["type"] = type;
            msg["product_ids"] = symbols;
            msg["channel"] = channel;
            msg["jwt"] = m_auth.createJwt();

            // Using a shared_ptr to manage the lifetime of the message string
            auto message_ptr = std::make_shared<std::string>(msg.dump());

            // 🚨 FIX: Use write queue instead of direct async_write to prevent soft_mutex crash
            enqueueWrite(message_ptr);
            sLog_Data(QString("📤 Queued subscription for %1 channel").arg(QString::fromStdString(channel)));
        };

        // Send messages for both level2 and market_trades channels
        sendMessage("level2", type, symbols);
        sendMessage("market_trades", type, symbols);
    });
}

// 🚨 FIX: Beast WebSocket write queue implementation
void MarketDataCore::enqueueWrite(std::shared_ptr<std::string> message) {
    // MUST be called from strand context only
    m_writeQueue.push_back(message);
    if (!m_writeInProgress) {
        doWrite();
    }
}

void MarketDataCore::doWrite() {
    // MUST be called from strand context only
    if (m_writeQueue.empty()) {
        m_writeInProgress = false;
        return;
    }
    
    m_writeInProgress = true;
    auto message = m_writeQueue.front();
    
    m_ws.async_write(net::buffer(*message),
        [this, message](beast::error_code ec, std::size_t) {
            if (ec) {
                sLog_Error(QString("WebSocket write error: %1").arg(QString::fromStdString(ec.message())));
                scheduleReconnect();
                return;
            }
            
            // Write completed - process next message
            m_writeQueue.pop_front();
            doWrite();  // Process next message or set m_writeInProgress = false
        });
}

void MarketDataCore::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;
    
    // 🚀 PHASE 2.1: Record message arrival time for latency analysis
    auto arrival_time = std::chrono::system_clock::now();
    
    std::string channel = message.value("channel", "");
    std::string type = message.value("type", "");
    
    if (channel == "market_trades") {
        handleMarketTrades(message, arrival_time);
    } else if (channel == "l2_data") {
        handleOrderBookData(message, arrival_time);
    } else if (channel == "subscriptions") {
        handleSubscriptionConfirmation(message);
    } else if (type == "error") {
        handleError(message);
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
        
        // Store in DataCache
        m_cache.addTrade(trade);
        
        // 🚀 PHASE 2.1: Record trade processed for throughput tracking
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
    std::string side = trade_data.value("side", "");
    trade.side = Cpp20Utils::fastSideDetection(side);
    
    // Parse timestamp from trade data or use exchange timestamp
    if (trade_data.contains("time")) {
        std::string trade_timestamp_str = trade_data["time"];
        trade.timestamp = Cpp20Utils::parseISO8601(trade_timestamp_str);
        
        // 🚀 PHASE 2.1: Record trade latency (exchange → arrival)
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
    // PHASE 1.4: Parse exchange timestamp from root-level JSON
    std::chrono::system_clock::time_point exchange_timestamp = std::chrono::system_clock::now();
    if (message.contains("timestamp")) {
        // Parse ISO8601 timestamp: "2023-02-09T20:32:50.714964855Z"
        std::string timestamp_str = message["timestamp"];
        exchange_timestamp = Cpp20Utils::parseISO8601(timestamp_str);
        
        // 🚀 PHASE 2.1: Record order book latency (exchange → arrival)
        if (m_monitor) {
            m_monitor->recordOrderBookLatency(exchange_timestamp, arrival_time);
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
            } else if (side == "offer" || side == "ask") {
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
    int updateCount = 0;
    
    for (const auto& update : event["updates"]) {
        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
            continue;
        }
        
        std::string side = update["side"];
        double price = Cpp20Utils::fastStringToDouble(update["price_level"].get<std::string>());
        double quantity = Cpp20Utils::fastStringToDouble(update["new_quantity"].get<std::string>());
        
        // Apply update to live order book (handles add/update/remove automatically)
        if (side == "offer") side = "ask"; // normalize
        m_cache.updateLiveOrderBook(product_id, side, price, quantity, exchange_timestamp);
        updateCount++;
    }
    
    // PHASE 2.1: Direct dense-only signal - NO CONVERSION!
    {
        QPointer<MarketDataCore> self(this);
        QString productIdQ = QString::fromStdString(product_id);
        QMetaObject::invokeMethod(this, [self, productIdQ]() {
            if (!self) return;
            emit self->liveOrderBookUpdated(productIdQ);
        }, Qt::QueuedConnection);
    }
    
    // 🚀 PHASE 2.1: Record order book update metrics
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

void MarketDataCore::handleSubscriptionConfirmation(const nlohmann::json& message) {
    std::string logMessage = std::format("✅ Subscription confirmed for {}",
        message.value("product_ids", nlohmann::json::array()).dump());
    sLog_Data(QString::fromStdString(logMessage));
}

void MarketDataCore::handleError(const nlohmann::json& message) {
    std::string logMessage = std::format("❌ Coinbase error: {}",
        message.value("message", "unknown error"));
    sLog_Error(QString::fromStdString(logMessage));
    QString err = message.contains("message") ? QString::fromStdString(message.at("message").get<std::string>())
                                               : QString("Provider error");
    emitError(err);
}

void MarketDataCore::replaySubscriptionsOnConnect() {
    if (m_products.empty()) return;
    auto symbols = m_products;
    sendSubscriptionMessage("subscribe", symbols);
}

void MarketDataCore::startHeartbeat() {
    m_pingTimer.expires_after(std::chrono::seconds(25));
    m_pingTimer.async_wait([this](beast::error_code ec){
        if (ec || !m_running) return;
        scheduleNextPing();
    });
}

void MarketDataCore::scheduleNextPing() {
    if (!m_ws.is_open()) {
        return;
    }
    m_ws.async_ping({}, [this](beast::error_code ec){
        if (ec) {
            std::cerr << "❌ Ping error: " << ec.message() << std::endl;
            emitError(QString("Ping error: %1").arg(QString::fromStdString(ec.message())));
            scheduleReconnect();
            return;
        }
        m_pingTimer.expires_after(std::chrono::seconds(25));
        m_pingTimer.async_wait([this](beast::error_code ec2){
            if (ec2 || !m_running) return;
            scheduleNextPing();
        });
    });
}