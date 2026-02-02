#include "SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>

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
    qRegisterMetaType<HeatmapHistoryColumn>("HeatmapHistoryColumn");
    qRegisterMetaType<QVector<HeatmapHistoryColumn>>("QVector<HeatmapHistoryColumn>");
    qRegisterMetaType<CandleBar>("CandleBar");
    qRegisterMetaType<QVector<CandleBar>>("QVector<CandleBar>");
}

SentinelStreamClient::~SentinelStreamClient() {
    disconnectFromServer();
}

void SentinelStreamClient::connectToServer() {
    if (m_running) return;
    m_ioc.restart();
    m_isConnected = false;
    m_writeQueue.clear();

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
    m_isConnected = false;
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
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::unsubscribe(const std::string& symbol) {
    nlohmann::json msg = {
        {"type", "unsubscribe"},
        {"symbol", symbol}
    };
    
    std::string str = msg.dump();
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::requestHeatmapHistory(const std::string& symbol,
                                                 int64_t timeframeMs,
                                                 int64_t endTimeMs,
                                                 int count) {
    if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
        return;
    }
    nlohmann::json msg = {
        {"type", "heatmap_history_request"},
        {"symbol", symbol},
        {"timeframe_ms", timeframeMs},
        {"end_time", endTimeMs},
        {"count", count}
    };

    std::string str = msg.dump();
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::requestCandleHistory(const std::string& symbol,
                                                int64_t timeframeSec,
                                                int64_t endTimeSec,
                                                int limit) {
    if (symbol.empty() || timeframeSec <= 0) {
        return;
    }
    nlohmann::json msg = {
        {"type", "candle_history_request"},
        {"symbol", symbol},
        {"timeframe_sec", timeframeSec},
        {"end_time_sec", endTimeSec},
        {"limit", limit}
    };

    std::string str = msg.dump();
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
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
    emit connected();
    
    doRead();

    // Flush pending writes
    net::post(m_strand, [this]() {
        if (!m_writeQueue.empty()) {
            doWrite();
        }
    });
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
    if (m_writeQueue.empty()) {
        return;
    }
    m_ws.async_write(net::buffer(m_writeQueue.front()),
                     [this](auto ec, auto bytes) { onWrite(ec, bytes); });
}

void SentinelStreamClient::onWrite(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        sLog_Error("Write failed: " << ec.message());
        return;
    }
    
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
        } else if (type == "heatmap_slice") {
             std::string symbol = msg.value("symbol", "");
             if (symbol.empty()) return;

             const int64_t startMs = msg.value("time_start", static_cast<int64_t>(0));
             const int64_t endMs = msg.value("time_end", static_cast<int64_t>(0));
             const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
             const int gridWidth = msg.value("grid_width", 0);
             const int gridHeight = msg.value("grid_height", 0);
             const double minPrice = msg.value("min_price", 0.0);
             const double maxPrice = msg.value("max_price", 0.0);
             const double tickSize = msg.value("tick_size", 0.0);
             const double midPrice = msg.value("mid_price", 0.0);
             const double lastTrade = msg.value("last_trade", 0.0);
             const bool reset = msg.value("reset", false);
             const std::string format = msg.value("format", "u8");
             const std::string encoded = msg.value("column", "");
             const std::string liquidityEncoded = msg.value("liquidity_column", "");
             const double liquidityScale = msg.value("liquidity_scale", 1.0);

             QByteArray column;
             if (!encoded.empty()) {
                 column = QByteArray::fromBase64(QByteArray::fromStdString(encoded));
             }
             QByteArray liquidityColumn;
             if (!liquidityEncoded.empty()) {
                 liquidityColumn = QByteArray::fromBase64(QByteArray::fromStdString(liquidityEncoded));
             }

             emit heatmapSliceReceived(QString::fromStdString(symbol),
                                       startMs,
                                       endMs,
                                       timeframeMs,
                                       gridWidth,
                                       gridHeight,
                                       minPrice,
                                       maxPrice,
                                       tickSize,
                                       midPrice,
                                       lastTrade,
                                       QString::fromStdString(format),
                                       column,
                                       liquidityColumn,
                                       liquidityScale,
                                       reset);
        } else if (type == "heatmap_history_chunk") {
             std::string symbol = msg.value("symbol", "");
             if (symbol.empty()) return;
             const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
             const int gridWidth = msg.value("grid_width", 0);
             const int gridHeight = msg.value("grid_height", 0);
             const std::string encoding = msg.value("encoding", "base64");
             const std::string liquidityEncoding = msg.value("liquidity_encoding", "base64");
             const auto columns = msg.value("columns", nlohmann::json::array());

             QVector<HeatmapHistoryColumn> out;
             if (columns.is_array()) {
                 out.reserve(static_cast<int>(columns.size()));
                 for (const auto& item : columns) {
                     HeatmapHistoryColumn col;
                     col.bucketStartMs = item.value("time_start", static_cast<int64_t>(0));
                     col.bucketEndMs = item.value("time_end", static_cast<int64_t>(0));
                     col.minPrice = item.value("min_price", 0.0);
                     col.maxPrice = item.value("max_price", 0.0);
                     col.tickSize = item.value("tick_size", 0.0);
                     const std::string encoded = item.value("column", "");
                     if (!encoded.empty() && encoding == "base64") {
                         col.intensity = QByteArray::fromBase64(QByteArray::fromStdString(encoded));
                     }
                     const std::string liqEncoded = item.value("liquidity_column", "");
                     if (!liqEncoded.empty() && liquidityEncoding == "base64") {
                         col.liquidity = QByteArray::fromBase64(QByteArray::fromStdString(liqEncoded));
                     }
                     col.liquidityScale = item.value("liquidity_scale", 1.0);
                     out.push_back(std::move(col));
                 }
             }

             emit heatmapHistoryReceived(QString::fromStdString(symbol),
                                         timeframeMs,
                                         gridWidth,
                                         gridHeight,
                                         out);
        } else if (type == "candle_history_chunk") {
             std::string symbol = msg.value("symbol", "");
             if (symbol.empty()) return;
             const int64_t timeframeSec = msg.value("timeframe_sec", static_cast<int64_t>(0));
             const int64_t startTimeSec = msg.value("start_time_sec", static_cast<int64_t>(0));
             const int64_t endTimeSec = msg.value("end_time_sec", static_cast<int64_t>(0));
             const auto candles = msg.value("candles", nlohmann::json::array());

             QVector<CandleBar> out;
             if (candles.is_array()) {
                 out.reserve(static_cast<int>(candles.size()));
                 for (const auto& item : candles) {
                     CandleBar bar;
                     bar.timeStartMs = item.value("time_start_ms", static_cast<int64_t>(0));
                     bar.timeEndMs = item.value("time_end_ms", static_cast<int64_t>(0));
                     bar.open = item.value("open", 0.0);
                     bar.high = item.value("high", 0.0);
                     bar.low = item.value("low", 0.0);
                     bar.close = item.value("close", 0.0);
                     bar.volume = item.value("volume", 0.0);
                     bar.isClosed = item.value("is_closed", false);
                     out.push_back(bar);
                 }
             }

             emit candleHistoryReceived(QString::fromStdString(symbol),
                                        timeframeSec,
                                        startTimeSec,
                                        endTimeSec,
                                        out);
        }

    } catch (const std::exception& e) {
        sLog_Error("Message parse error: " << e.what());
    }
}
