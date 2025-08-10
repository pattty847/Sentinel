#include "MarketDataCore.hpp"
#include "SentinelLogging.hpp"
#include "Cpp20Utils.hpp"  // 🚀 C++20 UTILITIES
#include <iostream>
#include <thread>
#include <chrono>
#include <QString>
#include <QMetaObject>
// 🚀 C++20 OPTIMIZATIONS
#include <format>    // std::format for efficient string formatting
#include <ranges>    // std::ranges for functional data processing

// Stub implementation for Phase 1 compilation verification

MarketDataCore::MarketDataCore(const std::vector<std::string>& products,
                               Authenticator& auth,
                               DataCache& cache)
    : QObject(nullptr)
    , m_products(products)
    , m_auth(auth)
    , m_cache(cache)
{
    // Configure SSL context
    m_sslCtx.set_default_verify_paths();
    m_sslCtx.set_verify_mode(ssl::verify_peer);
    
    sLog_App(QString("🏗️ MarketDataCore initialized for %1 products").arg(products.size()));
}

MarketDataCore::~MarketDataCore() {
    stop();
    sLog_App("🏗️ MarketDataCore destroyed");
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
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🔗 TCP connected, starting SSL handshake...");
    
    // Set SNI hostname for SSL
    if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), m_host.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "❌ SSL SNI error: " << ssl_ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    
    // Set hostname verification for SSL
    SSL* ssl_handle = m_ws.next_layer().native_handle();
    if (SSL_set1_host(ssl_handle, m_host.c_str()) != 1) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "❌ SSL hostname verification setup error: " << ssl_ec.message() << std::endl;
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
        scheduleReconnect();
        return;
    }
    
    sLog_Data("🌐 WebSocket connected! Sending subscriptions...");
    
    // Send level2 subscription first
    std::string level2_sub = buildSubscribe("level2");
    sLog_Data("📤 Sending level2 subscription...");
    
    m_ws.async_write(net::buffer(level2_sub),
        [this](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "❌ Level2 subscription write error: " << ec.message() << std::endl;
                scheduleReconnect();
                return;
            }
            
            sLog_Data("✅ Level2 subscription sent! Sending trades subscription...");
            
            // Send market_trades subscription
            std::string trades_sub = buildSubscribe("market_trades");
            m_ws.async_write(net::buffer(trades_sub),
                beast::bind_front_handler(&MarketDataCore::onWrite, this));
        });
}

void MarketDataCore::onWrite(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "❌ Write error: " << ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    
    sLog_Data("✅ All subscriptions sent! Starting to read messages...");
    
    // Start reading messages
    m_ws.async_read(m_buf,
        beast::bind_front_handler(&MarketDataCore::onRead, this));
}

void MarketDataCore::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "❌ Read error: " << ec.message() << std::endl;
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
    
    // Reset the stream state
    if (m_ws.is_open()) {
        doClose();
    }
    
    // NON-BLOCKING timer-based reconnect
    m_reconnectTimer.expires_after(delay);
    m_reconnectTimer.async_wait([this](beast::error_code ec) {
        if (ec || !m_running) return;
        
        sLog_Data("🔄 Attempting reconnection...");
        m_resolver.async_resolve(m_host, m_port,
            beast::bind_front_handler(&MarketDataCore::onResolve, this));
    });
}

std::string MarketDataCore::buildSubscribe(const std::string& channel) const {
    nlohmann::json sub;
    sub["type"] = "subscribe";
    sub["product_ids"] = m_products;
    sub["channel"] = channel;
    sub["jwt"] = m_auth.createJwt();
    
    std::string result = sub.dump();
    sLog_Data(QString("🔐 %1 subscription: %2").arg(QString::fromStdString(channel)).arg(QString::fromStdString(result)));
    return result;
}

void MarketDataCore::dispatch(const nlohmann::json& message) {
    if (!message.is_object()) return;
    
    std::string channel = message.value("channel", "");
    std::string type = message.value("type", "");
    
    if (channel == "market_trades") {
        // Process trade data
        if (message.contains("events")) {
            for (const auto& event : message["events"]) {
                if (event.contains("trades")) {
                    for (const auto& trade_data : event["trades"]) {
                        // Create Trade object
                        Trade trade;
                        trade.product_id = trade_data.value("product_id", "");
                        trade.trade_id = trade_data.value("trade_id", "");
                        trade.price = Cpp20Utils::fastStringToDouble(trade_data.value("price", "0"));
                        trade.size = Cpp20Utils::fastStringToDouble(trade_data.value("size", "0"));
                        
                        // Parse side
                        std::string side = trade_data.value("side", "");
                        trade.side = Cpp20Utils::fastSideDetection(side);
                        
                        // Parse timestamp (for now use current time)
                        trade.timestamp = std::chrono::system_clock::now();
                        
                        // Store in DataCache
                        m_cache.addTrade(trade);
                        
                        // 🔥 NEW: Emit real-time signal to GUI layer (Qt thread-safe)
                        Trade tradeCopy = trade; // Make a copy for thread safety
                        QMetaObject::invokeMethod(this, [this, tradeCopy]() {
                            emit tradeReceived(tradeCopy);
                        }, Qt::QueuedConnection);
                        
                        // 🔥 THROTTLED LOGGING: Only log every 20th trade to reduce spam
                        if (++m_tradeLogCount % 20 == 1) { // Log 1st, 21st, 41st trade, etc.
                            // 🚀 C++20: Use optimized formatting from utils
                            std::string logMessage = Cpp20Utils::formatTradeLog(
                                trade.product_id, trade.price, trade.size, side, m_tradeLogCount.load());
                            sLog_Data(QString::fromStdString(logMessage));
                        }
                    }
                }
            }
        }
    } else if (channel == "l2_data") {
        // 🔥 NEW: STATEFUL ORDER BOOK PROCESSING
        if (message.contains("events")) {
            for (const auto& event : message["events"]) {
                std::string eventType = event.value("type", "");
                std::string product_id = event.value("product_id", "");
                
                if (eventType == "snapshot" && event.contains("updates") && !product_id.empty()) {
                    // 🏗️ SNAPSHOT: Initialize complete order book state
                    OrderBook snapshot;
                    snapshot.product_id = product_id;
                    snapshot.timestamp = std::chrono::system_clock::now();
                    
                    // Process snapshot data
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
                                snapshot.bids.push_back(level);
                            } else if (side == "offer") {
                                snapshot.asks.push_back(level);
                            }
                        }
                    }
                    
                    // Initialize the live order book with complete state
                    m_cache.initializeLiveOrderBook(product_id, snapshot);
                    
                    // 🚀 C++20: Use optimized formatting from utils
                    std::string logMessage = Cpp20Utils::formatOrderBookLog(
                        product_id, snapshot.bids.size(), snapshot.asks.size());
                    sLog_Data(QString::fromStdString(logMessage));
                    
                } else if (eventType == "update" && event.contains("updates") && !product_id.empty()) {
                    // 🔄 UPDATE: Apply incremental changes to stateful order book
                    int updateCount = 0;
                    
                    for (const auto& update : event["updates"]) {
                        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
                            continue;
                        }
                        
                        std::string side = update["side"];
                        double price = Cpp20Utils::fastStringToDouble(update["price_level"].get<std::string>());
                        double quantity = Cpp20Utils::fastStringToDouble(update["new_quantity"].get<std::string>());
                        
                        // Apply update to live order book (handles add/update/remove automatically)
                        m_cache.updateLiveOrderBook(product_id, side, price, quantity);
                        updateCount++;
                    }
                    
                    // 🔥 NEW: Emit real-time order book signal to GUI layer (Qt thread-safe)
                    auto liveBook = m_cache.getLiveOrderBook(product_id);
                    OrderBook orderBookCopy = liveBook; // Make a copy for thread safety
                    QMetaObject::invokeMethod(this, [this, orderBookCopy]() {
                        emit orderBookUpdated(orderBookCopy);
                    }, Qt::QueuedConnection);
                    
                    // 🔥 THROTTLED LOGGING: Only log every 1000th update to reduce spam
                    if (++m_orderBookLogCount % 1000 == 1) {
                        // 🚀 C++20: Use optimized formatting from utils
                        std::string logMessage = Cpp20Utils::formatOrderBookLog(
                            product_id, liveBook.bids.size(), liveBook.asks.size(), updateCount);
                        sLog_Data(QString::fromStdString(logMessage));
                    }
                }
            }
        }
    } else if (channel == "subscriptions") {
        // 🚀 C++20: Use std::format for faster string formatting
        std::string logMessage = std::format("✅ Subscription confirmed for {}",
            message.value("product_ids", nlohmann::json::array()).dump());
        sLog_Data(QString::fromStdString(logMessage));
    } else if (type == "error") {
        // 🚀 C++20: Use std::format for faster string formatting
        std::string logMessage = std::format("❌ Coinbase error: {}",
            message.value("message", "unknown error"));
        sLog_Error(QString::fromStdString(logMessage));
    }
} 