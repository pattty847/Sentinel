#include "MarketDataCore.hpp"
#include <iostream>
#include <thread>
#include <chrono>

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
    
    std::cout << "🏗️ MarketDataCore initialized for " << products.size() << " products" << std::endl;
}

MarketDataCore::~MarketDataCore() {
    stop();
    std::cout << "🏗️ MarketDataCore destroyed" << std::endl;
}

void MarketDataCore::start() {
    if (!m_running.exchange(true)) {
        std::cout << "🚀 Starting MarketDataCore..." << std::endl;
        m_ioThread = std::thread(&MarketDataCore::run, this);
    }
}

void MarketDataCore::stop() {
    if (m_running.exchange(false)) {
        std::cout << "🛑 Stopping MarketDataCore..." << std::endl;
        
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
        
        std::cout << "✅ MarketDataCore stopped" << std::endl;
    }
}

void MarketDataCore::run() {
    std::cout << "🔌 Starting connection to " << m_host << ":" << m_port << std::endl;
    
    // Start the connection process
    m_resolver.async_resolve(m_host, m_port,
        beast::bind_front_handler(&MarketDataCore::onResolve, this));
    
    // Run the IO context - this blocks until stopped
    m_ioc.run();
    
    std::cout << "🔌 IO context stopped" << std::endl;
}

void MarketDataCore::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "❌ Resolve error: " << ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    
    std::cout << "🔍 DNS resolved, connecting..." << std::endl;
    
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
    
    std::cout << "🔗 TCP connected, starting SSL handshake..." << std::endl;
    
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
    
    std::cout << "🔐 SSL handshake complete, starting WebSocket handshake..." << std::endl;
    
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
    
    std::cout << "🌐 WebSocket connected! Sending subscriptions..." << std::endl;
    
    // Send level2 subscription first
    std::string level2_sub = buildSubscribe("level2");
    std::cout << "📤 Sending level2 subscription..." << std::endl;
    
    m_ws.async_write(net::buffer(level2_sub),
        [this](beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "❌ Level2 subscription write error: " << ec.message() << std::endl;
                scheduleReconnect();
                return;
            }
            
            std::cout << "✅ Level2 subscription sent! Sending trades subscription..." << std::endl;
            
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
    
    std::cout << "✅ All subscriptions sent! Starting to read messages..." << std::endl;
    
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
        std::cout << "🔌 Closing WebSocket connection..." << std::endl;
        m_ws.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&MarketDataCore::onClose, this));
    }
}

void MarketDataCore::onClose(beast::error_code ec) {
    if (ec) {
        std::cerr << "❌ Close error: " << ec.message() << std::endl;
    } else {
        std::cout << "✅ WebSocket connection closed" << std::endl;
    }
}

void MarketDataCore::scheduleReconnect() {
    if (!m_running) return;
    
    std::cout << "🔄 Scheduling reconnect in 5 seconds..." << std::endl;
    
    // Reset the stream state
    if (m_ws.is_open()) {
        doClose();
    }
    
    // Schedule reconnection
    net::post(m_ioc, [this]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (m_running) {
            std::cout << "🔄 Attempting reconnection..." << std::endl;
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
    std::cout << "🔐 " << channel << " subscription: " << result << std::endl;
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
                            std::cout << "💰 " << trade.product_id << ": $" << trade.price 
                                      << " size:" << trade.size << " (" << side << ")" 
                                      << " [" << tradeLogCount << " trades total]" << std::endl;
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
                    
                    std::cout << "📸 SNAPSHOT: Initialized " << product_id << " with " 
                              << snapshot.bids.size() << " bids, " << snapshot.asks.size() << " asks" << std::endl;
                    
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
                        std::cout << "🏭 LIVE UPDATE " << liveBookLogCount << ": " << product_id 
                                  << " → " << liveBook.bids.size() << " bids, " 
                                  << liveBook.asks.size() << " asks (+" << updateCount << " changes)" << std::endl;
                    }
                }
            }
        }
    } else if (channel == "subscriptions") {
        std::cout << "✅ Subscription confirmed for " << message.value("product_ids", nlohmann::json::array()).dump() << std::endl;
    } else if (type == "error") {
        std::cout << "❌ Coinbase error: " << message.value("message", "unknown error") << std::endl;
    }
} 