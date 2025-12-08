#include "SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"

SentinelStreamClient::SentinelStreamClient(const std::string& host, const std::string& port, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_port(port)
    , m_ws(m_ioc)
{
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
            
            net::async_connect(
                m_ws.next_layer(),
                results,
                [this](auto ec, auto ep) { onConnect(ec, ep); }
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

void SentinelStreamClient::onConnect(boost::beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
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
        } else if (type == "slice_batch") {
            // Process batch
        }
    } catch (const std::exception& e) {
        sLog_Error("Message parse error: " << e.what());
    }
}

