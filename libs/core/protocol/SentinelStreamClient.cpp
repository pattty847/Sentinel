#include "SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>
#include <QElapsedTimer>

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
    qRegisterMetaType<HeatmapSlice>("HeatmapSlice");
    qRegisterMetaType<CandleBar>("CandleBar");
    qRegisterMetaType<QVector<CandleBar>>("QVector<CandleBar>");
    qRegisterMetaType<ServerConfig>("ServerConfig");
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

        if (type == "server_config") {
            ServerConfig cfg;
            const int schema = msg.value("schema_version", 0);
            if (schema != 1) {
                sLog_Warning("server_config schema version unsupported: " << schema);
            }
            if (msg.contains("timeframes_ms") && msg["timeframes_ms"].is_array()) {
                cfg.heatmap.timeframesMs.clear();
                for (const auto& item : msg["timeframes_ms"]) {
                    const int64_t tf = item.get<int64_t>();
                    if (tf > 0) {
                        cfg.heatmap.timeframesMs.push_back(tf);
                    }
                }
            }
            if (msg.contains("heatmap") && msg["heatmap"].is_object()) {
                const auto& hm = msg["heatmap"];
                cfg.heatmap.gridWidth = hm.value("grid_width", cfg.heatmap.gridWidth);
                cfg.heatmap.gridHeight = hm.value("grid_height", cfg.heatmap.gridHeight);
                cfg.heatmap.tickSize = hm.value("tick_size", cfg.heatmap.tickSize);
                cfg.heatmap.recenterDelta = hm.value("recenter_delta", cfg.heatmap.recenterDelta);
                cfg.heatmap.bandFast = hm.value("band_fast", cfg.heatmap.bandFast);
                cfg.heatmap.bandMedium = hm.value("band_medium", cfg.heatmap.bandMedium);
                cfg.heatmap.bandSlow = hm.value("band_slow", cfg.heatmap.bandSlow);
                cfg.heatmap.intensityMode = hm.value("intensity_mode", cfg.heatmap.intensityMode);
                cfg.heatmap.intensityMaxMode = hm.value("intensity_max_mode", cfg.heatmap.intensityMaxMode);
                cfg.heatmap.intensityMaxDecay = hm.value("intensity_max_decay", cfg.heatmap.intensityMaxDecay);
                cfg.heatmap.intensityLogScale = hm.value("intensity_log_scale", cfg.heatmap.intensityLogScale);
                cfg.heatmap.intensityPower = hm.value("intensity_power", cfg.heatmap.intensityPower);
                cfg.heatmap.intensityFloor = hm.value("intensity_floor", cfg.heatmap.intensityFloor);
                cfg.heatmap.activeTimeframeMs = hm.value("active_timeframe_ms", cfg.heatmap.activeTimeframeMs);
            }
            if (msg.contains("orderbook") && msg["orderbook"].is_object()) {
                const auto& ob = msg["orderbook"];
                cfg.orderbook.tickSize = ob.value("tick_size", cfg.orderbook.tickSize);
                cfg.orderbook.bandPct = ob.value("band_pct", cfg.orderbook.bandPct);
            }
            if (msg.contains("candles") && msg["candles"].is_object()) {
                const auto& cd = msg["candles"];
                cfg.candles.bpsFast = cd.value("update_bps_fast", cfg.candles.bpsFast);
                cfg.candles.bpsSlow = cd.value("update_bps_slow", cfg.candles.bpsSlow);
                cfg.candles.tickMultFast = cd.value("update_tick_mult_fast", cfg.candles.tickMultFast);
                cfg.candles.tickMultSlow = cd.value("update_tick_mult_slow", cfg.candles.tickMultSlow);
                cfg.candles.silenceMsFast = cd.value("update_silence_ms_fast", cfg.candles.silenceMsFast);
                cfg.candles.silenceMsSlow = cd.value("update_silence_ms_slow", cfg.candles.silenceMsSlow);
                cfg.candles.volumeFast = cd.value("update_volume_fast", cfg.candles.volumeFast);
                cfg.candles.volumeSlow = cd.value("update_volume_slow", cfg.candles.volumeSlow);
                cfg.candles.tickSize = cd.value("update_tick_size", cfg.candles.tickSize);
            }
            if (msg.contains("default_symbols") && msg["default_symbols"].is_array()) {
                cfg.defaultSymbols.clear();
                for (const auto& item : msg["default_symbols"]) {
                    if (item.is_string()) {
                        cfg.defaultSymbols.push_back(item.get<std::string>());
                    }
                }
            }

            emit serverConfigReceived(cfg);
        } else if (type == "snapshot") {
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

             if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                 static QElapsedTimer timer;
                 static bool started = false;
                 if (!started) {
                     timer.start();
                     started = true;
                 }
                 if (timer.elapsed() > 1000) {
                     sLog_Debug(QString("Heatmap slice recv: symbol=%1 tfMs=%2 start=%3 end=%4 grid=%5x%6")
                                .arg(QString::fromStdString(symbol))
                                .arg(timeframeMs)
                                .arg(startMs)
                                .arg(endMs)
                                .arg(gridWidth)
                                .arg(gridHeight));
                     timer.restart();
                 }
             }

             HeatmapSlice slice;
             slice.symbol = QString::fromStdString(symbol);
             slice.bucketStartMs = startMs;
             slice.bucketEndMs = endMs;
             slice.timeframeMs = timeframeMs;
             slice.gridWidth = gridWidth;
             slice.gridHeight = gridHeight;
             slice.minPrice = minPrice;
             slice.maxPrice = maxPrice;
             slice.tickSize = tickSize;
             slice.midPrice = midPrice;
             slice.lastTrade = lastTrade;
             slice.format = QString::fromStdString(format);
             slice.column = column;
             slice.liquidityColumn = liquidityColumn;
             slice.liquidityScale = liquidityScale;
             slice.reset = reset;
             emit heatmapSliceReceived(slice);
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

             if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                 const int count = out.size();
                 const int64_t first = count > 0 ? out.front().bucketStartMs : 0;
                 const int64_t last = count > 0 ? out.back().bucketStartMs : 0;
                 sLog_Debug(QString("Heatmap history recv: symbol=%1 tfMs=%2 count=%3 first=%4 last=%5")
                            .arg(QString::fromStdString(symbol))
                            .arg(timeframeMs)
                            .arg(count)
                            .arg(first)
                            .arg(last));
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

             if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                 const int count = out.size();
                 const int64_t first = count > 0 ? out.front().timeStartMs : 0;
                 const int64_t last = count > 0 ? out.back().timeStartMs : 0;
                 sLog_Debug(QString("Candle history recv: symbol=%1 tfSec=%2 count=%3 first=%4 last=%5 startSec=%6 endSec=%7")
                            .arg(QString::fromStdString(symbol))
                            .arg(timeframeSec)
                            .arg(count)
                            .arg(first)
                            .arg(last)
                            .arg(startTimeSec)
                            .arg(endTimeSec));
             }

             emit candleHistoryReceived(QString::fromStdString(symbol),
                                        timeframeSec,
                                        startTimeSec,
                                        endTimeSec,
                                        out);
        } else if (type == "candle_bar_update" || type == "candle_bar_closed") {
             std::string symbol = msg.value("symbol", "");
             if (symbol.empty()) return;
             const int64_t timeframeSec = msg.value("timeframe_sec", static_cast<int64_t>(0));
             const int64_t bucketStartMs = msg.value("bucket_start_ms", static_cast<int64_t>(0));
             const int64_t seq = msg.value("seq", static_cast<int64_t>(0));
             const auto item = msg.value("candle", nlohmann::json::object());

             CandleBar bar;
             bar.timeStartMs = item.value("time_start_ms", static_cast<int64_t>(0));
             bar.timeEndMs = item.value("time_end_ms", static_cast<int64_t>(0));
             bar.open = item.value("open", 0.0);
             bar.high = item.value("high", 0.0);
             bar.low = item.value("low", 0.0);
             bar.close = item.value("close", 0.0);
             bar.volume = item.value("volume", 0.0);
             bar.isClosed = item.value("is_closed", false);

             const auto symbolQ = QString::fromStdString(symbol);
             if (type == "candle_bar_closed") {
                 emit candleBarClosedReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar);
             } else {
                 emit candleBarUpdateReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar);
             }
        }

    } catch (const std::exception& e) {
        sLog_Error("Message parse error: " << e.what());
    }
}
