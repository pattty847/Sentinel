#include "MarketDataCore.hpp"
#include "SentinelLogging.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <QString>

// Stub implementation for Phase 1 compilation verification

MarketDataCore::MarketDataCore(const std::vector<std::string>& products,
                               Authenticator& auth,
                               DataCache& cache)
    : m_products(products)
    , m_auth(auth)
    , m_cache(cache)
{
    // Configure SSL context
    m_sslCtx.set_default_verify_paths();
    m_sslCtx.set_verify_mode(ssl::verify_peer);
    
    sLog_Init(QString("🏗️ MarketDataCore initialized for %1 products").arg(products.size()));
}

MarketDataCore::~MarketDataCore() {
    stop();
    sLog_Init("🏗️ MarketDataCore destroyed");
}

void MarketDataCore::start() {
    if (!m_running.exchange(true)) {
        sLog_Init("🚀 Starting MarketDataCore...");
        m_ioThread = std::thread(&MarketDataCore::run, this);
    }
}

void MarketDataCore::stop() {
    if (m_running.exchange(false)) {
        sLog_Init("🛑 Stopping MarketDataCore...");
        
        // Close WebSocket gracefully
        if (m_ws.is_open()) {
            net::post(m_ioc, [this]() { doClose(); });
        }
        
        // Stop IO context
        m_ioc.stop();
        
        // Join thread
        if (m_ioThread.joinable()) {
            m_ioThread.join();
        }
        
        sLog_Init("✅ MarketDataCore stopped");
    }
}

void MarketDataCore::run() {
    sLog_Connection(QString("🔌 Starting connection to %1:%2").arg(QString::fromStdString(m_host)).arg(QString::fromStdString(m_port)));
    
    // Start the connection process
    m_resolver.async_resolve(m_host, m_port,
        beast::bind_front_handler(&MarketDataCore::onResolve, this));
    
    // Run the IO context - this blocks until stopped
    m_ioc.run();
    
    sLog_Connection("🔌 IO context stopped");
}

void MarketDataCore::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "❌ Resolve error: " << ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    
    sLog_Connection("🔍 DNS resolved, connecting...");
    
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
    
    sLog_Connection("🔗 TCP connected, starting SSL handshake...");
    
    // Set SNI hostname for SSL
    if (!SSL_set_tlsext_host_name(m_ws.next_layer().native_handle(), m_host.c_str())) {
        beast::error_code ssl_ec(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        std::cerr << "❌ SSL SNI error: " << ssl_ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    
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
    
    sLog_Connection("🔐 SSL handshake complete, starting WebSocket handshake...");
    
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
    
    sLog_Connection("🌐 WebSocket connected! Sending subscriptions...");
    
    // Send level2 subscription first
    std::string level2_sub = buildSubscribe("level2");
    sLog_Subscription("📤 Sending level2 subscription...");
    
    m_ws.async_write(net::buffer(level2_sub),
        [this](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "❌ Level2 subscription write error: " << ec.message() << std::endl;
                scheduleReconnect();
                return;
            }
            
            sLog_Subscription("✅ Level2 subscription sent! Sending trades subscription...");
            
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
    
    sLog_Connection("✅ All subscriptions sent! Starting to read messages...");
    
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
        sLog_Connection("🔌 Closing WebSocket connection...");
        m_ws.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&MarketDataCore::onClose, this));
    }
}

void MarketDataCore::onClose(beast::error_code ec) {
    if (ec) {
        std::cerr << "❌ Close error: " << ec.message() << std::endl;
    } else {
        sLog_Connection("✅ WebSocket connection closed");
    }
}

void MarketDataCore::scheduleReconnect() {
    if (!m_running) return;
    
    sLog_Connection("🔄 Scheduling reconnect in 5 seconds...");
    
    // Reset the stream state
    if (m_ws.is_open()) {
        doClose();
    }
    
    // Schedule reconnection
    net::post(m_ioc, [this]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (m_running) {
            sLog_Connection("🔄 Attempting reconnection...");
            m_resolver.async_resolve(m_host, m_port,
                beast::bind_front_handler(&MarketDataCore::onResolve, this));
        }
    });
}

std::string MarketDataCore::buildSubscribe(const std::string& channel) const {
    nlohmann::json sub;
    sub["type"] = "subscribe";
    sub["product_ids"] = m_products;
    sub["channel"] = channel;
    sub["jwt"] = m_auth.createJwt();
    
    std::string result = sub.dump();
    sLog_Subscription(QString("🔐 %1 subscription: %2").arg(QString::fromStdString(channel)).arg(QString::fromStdString(result)));
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
                        trade.price = std::stod(trade_data.value("price", "0"));
                        trade.size = std::stod(trade_data.value("size", "0"));
                        
                        // Parse side
                        std::string side = trade_data.value("side", "");
                        if (side == "BUY") {
                            trade.side = AggressorSide::Buy;
                        } else if (side == "SELL") {
                            trade.side = AggressorSide::Sell;
                        } else {
                            trade.side = AggressorSide::Unknown;
                        }
                        
                        // Parse timestamp (for now use current time)
                        trade.timestamp = std::chrono::system_clock::now();
                        
                        // Store in DataCache
                        m_cache.addTrade(trade);
                        
                        // 🔥 THROTTLED LOGGING: Only log every 20th trade to reduce spam
                        static int tradeLogCount = 0;
                        if (++tradeLogCount % 20 == 1) { // Log 1st, 21st, 41st trade, etc.
                            sLog_Trades(QString("💰 %1: $%2 size:%3 (%4) [%5 trades total]")
                                        .arg(QString::fromStdString(trade.product_id))
                                        .arg(trade.price).arg(trade.size)
                                        .arg(QString::fromStdString(side)).arg(tradeLogCount));
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
                        double price = std::stod(update["price_level"].get<std::string>());
                        double quantity = std::stod(update["new_quantity"].get<std::string>());
                        
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
                    
                    sLog_Cache(QString("📸 SNAPSHOT: Initialized %1 with %2 bids, %3 asks")
                               .arg(QString::fromStdString(product_id))
                               .arg(snapshot.bids.size()).arg(snapshot.asks.size()));
                    
                } else if (eventType == "update" && event.contains("updates") && !product_id.empty()) {
                    // 🔄 UPDATE: Apply incremental changes to stateful order book
                    int updateCount = 0;
                    
                    for (const auto& update : event["updates"]) {
                        if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
                            continue;
                        }
                        
                        std::string side = update["side"];
                        double price = std::stod(update["price_level"].get<std::string>());
                        double quantity = std::stod(update["new_quantity"].get<std::string>());
                        
                        // Apply update to live order book (handles add/update/remove automatically)
                        m_cache.updateLiveOrderBook(product_id, side, price, quantity);
                        updateCount++;
                    }
                    
                    // 🔥 THROTTLED LOGGING: Only log every 100th update to reduce spam
                    static int liveBookLogCount = 0;
                    if (++liveBookLogCount % 100 == 1) {
                        auto liveBook = m_cache.getLiveOrderBook(product_id);
                        sLog_Cache(QString("🏭 LIVE UPDATE %1: %2 → %3 bids, %4 asks (+%5 changes)")
                                   .arg(liveBookLogCount).arg(QString::fromStdString(product_id))
                                   .arg(liveBook.bids.size()).arg(liveBook.asks.size()).arg(updateCount));
                    }
                }
            }
        }
    } else if (channel == "subscriptions") {
        sLog_Subscription(QString("✅ Subscription confirmed for %1").arg(QString::fromStdString(message.value("product_ids", nlohmann::json::array()).dump())));
    } else if (type == "error") {
        sLog_Error(QString("❌ Coinbase error: %1").arg(QString::fromStdString(message.value("message", "unknown error"))));
    }
} 