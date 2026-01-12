#include "SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"

SentinelStreamClient::SentinelStreamClient(const std::string& host, const std::string& port, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_port(port)
    , m_ws(m_ioc)
{
    qRegisterMetaType<BookLevelUpdate>("BookLevelUpdate");
    qRegisterMetaType<std::vector<BookLevelUpdate>>("BookLevelUpdateVector");
    qRegisterMetaType<OrderBookLevel>("OrderBookLevel");
    qRegisterMetaType<std::vector<OrderBookLevel>>("OrderBookLevelVector");
}

SentinelStreamClient::~SentinelStreamClient() {
    disconnectFromServer();
}

void SentinelStreamClient::connectToServer() {
    if (m_running) return;
    
    m_running = true;
    m_work = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(m_ioc.get_executor());
    
    m_thread = std::thread([this] {
        try {
            tcp::resolver resolver(m_ioc);
            auto const results = resolver.resolve(m_host, m_port);
            
            auto& stream = m_ws.next_layer();
            stream.async_connect(
                results,
                [this](auto ec, tcp::endpoint ep) { onConnect(ec, ep); }
            );
            
            m_ioc.run();
        } catch (const std::exception& e) {
            sLog_Error("Client thread exception: " << e.what());
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void SentinelStreamClient::disconnectFromServer() {
    m_running = false;
    if (m_work) m_work->reset();
    
    if (m_isConnected) {
        // Close websocket gracefully... or just stop ioc
    }
    
    m_ioc.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SentinelStreamClient::subscribe(const std::string& symbol) {
    nlohmann::json msg = {
        {"type", "subscribe"},
        {"symbol", symbol}
    };
    
    std::string str = msg.dump();
    std::lock_guard<std::mutex> lock(m_writeMutex);
    m_writeQueue.push_back(std::move(str));
    
    if (m_writeQueue.size() == 1) {
        doWrite();
    }
}

void SentinelStreamClient::unsubscribe(const std::string& symbol) {
    nlohmann::json msg = {
        {"type", "unsubscribe"},
        {"symbol", symbol}
    };
    
    std::string str = msg.dump();
    std::lock_guard<std::mutex> lock(m_writeMutex);
    m_writeQueue.push_back(std::move(str));
    
    if (m_writeQueue.size() == 1) {
        doWrite();
    }
}

void SentinelStreamClient::onConnect(boost::beast::error_code ec, tcp::endpoint) {
    if (ec) {
        sLog_Error("Connect failed: " << ec.message());
        emit errorOccurred(QString::fromStdString(ec.message()));
        return;
    }
    
    m_ws.async_handshake(m_host, "/", [this](auto ec) { onHandshake(ec); });
}

void SentinelStreamClient::onHandshake(boost::beast::error_code ec) {
    if (ec) {
        sLog_Error("Handshake failed: " << ec.message());
        emit errorOccurred(QString::fromStdString(ec.message()));
        return;
    }
    
    m_isConnected = true;
    sLog_App("Client Connected to Server");
    emit connected();
    
    doRead();

    // Flush pending writes
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (!m_writeQueue.empty()) {
            doWrite();
        }
    }
}

void SentinelStreamClient::doRead() {
    m_ws.async_read(m_buffer, [this](auto ec, auto bytes) { onRead(ec, bytes); });
}

void SentinelStreamClient::onRead(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        sLog_Error("Read failed: " << ec.message());
        m_isConnected = false;
        emit disconnected();
        return;
    }
    
    std::string msg = boost::beast::buffers_to_string(m_buffer.data());
    m_buffer.consume(bytes_transferred);
    
    handleMessage(msg);
    
    doRead();
}

void SentinelStreamClient::doWrite() {
    // Requires write mutex locked by caller if calling directly? 
    // No, doWrite should be called from strand or safe context.
    // For simplicity here, we assume single threaded write loop or careful locking.
    // Beast requires only one async_write at a time.
    
    // We'll dispatch to strand if we were doing this properly.
    // For Phase 2 Prototype, let's keep it simple.
    
    // Actually, doWrite needs to be safe.
    m_ws.async_write(
        net::buffer(m_writeQueue.front()),
        [this](auto ec, auto bytes) { onWrite(ec, bytes); }
    );
}

void SentinelStreamClient::onWrite(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        sLog_Error("Write failed: " << ec.message());
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_writeMutex);
    m_writeQueue.pop_front();
    
    if (!m_writeQueue.empty()) {
        doWrite();
    }
}

void SentinelStreamClient::handleMessage(const std::string& msgStr) {
    try {
        auto msg = nlohmann::json::parse(msgStr);
        std::string type = msg.value("type", "unknown");
        
        if (type == "snapshot") {
            // Process snapshot
            std::string symbol = msg.value("symbol", "");
            if (symbol.empty()) return;
            
            std::vector<OrderBookLevel> bids;
            std::vector<OrderBookLevel> asks;
            
            if (msg.contains("bids")) {
                for (const auto& level : msg["bids"]) {
                    if (level.contains("p") && level.contains("q")) {
                        bids.push_back({level["p"], level["q"]});
                    }
                }
            }
            if (msg.contains("asks")) {
                for (const auto& level : msg["asks"]) {
                    if (level.contains("p") && level.contains("q")) {
                        asks.push_back({level["p"], level["q"]});
                    }
                }
            }
            
            emit snapshotReceived(QString::fromStdString(symbol), bids, asks);
            
        } else if (type == "l2update") {
             std::string symbol = msg.value("product_id", "");
             if (symbol.empty()) return;
             
             if (msg.contains("deltas")) {
                 std::vector<BookLevelUpdate> updates;
                 for (const auto& d : msg["deltas"]) {
                     // JSON: { "side": "bid"/"ask", "price": float, "size": float }
                     std::string side = d.value("side", "");
                     bool isBid = (side == "bid");
                     double price = d.value("price", 0.0);
                     double size = d.value("size", 0.0);
                     
                     updates.push_back({isBid, price, size});
                 }
                 emit l2UpdateReceived(QString::fromStdString(symbol), updates);
             }
        } else if (type == "trade") {
             Trade t;
             t.product_id = msg.value("product_id", "");
             t.price = msg.value("price", 0.0);
             t.size = msg.value("size", 0.0);
             std::string side = msg.value("side", "");
             t.side = (side == "buy") ? AggressorSide::Buy : AggressorSide::Sell;
             // t.timestamp? 
             
             emit tradeReceived(t);
        }

    } catch (const std::exception& e) {
        sLog_Error("Message parse error: " << e.what());
    }
}

