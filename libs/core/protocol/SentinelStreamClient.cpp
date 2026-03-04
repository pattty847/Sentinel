#include "SentinelStreamClient.hpp"
#include "SentinelLogging.hpp"
#include "ProtocolValidation.hpp"
#include "SentinelStreamClientParseHelpers.hpp"
#include "VolumeProfileSlice.hpp"
#include <QByteArray>
#include <QElapsedTimer>
#include <array>
#include <cstdint>

namespace {

constexpr qint64 kSchemaLogThrottleMs = 2000;

enum class DropReason : int {
    ServerSchema = 0,
    HeatmapSchema,
    CandleSchema,
    FootprintSchema,
    HeatmapGridHeight,
    HeatmapPayloadEstimate,
    HeatmapPayloadDecoded,
    HeatmapBase64Decode,
    HeatmapHistoryGridHeight,
    HeatmapHistoryPayloadEstimate,
    HeatmapHistoryPayloadDecoded,
    HeatmapHistoryBase64Decode,
    FootprintGridHeight,
    FootprintPayloadEstimate,
    FootprintPayloadDecoded,
    FootprintBase64Decode,
    FootprintSliceMeta,
    FootprintPayloadShape,
    TpoSchema,
    TpoGridHeight,
    TpoPayloadEstimate,
    TpoPayloadDecoded,
    TpoBase64Decode,
    VolumeProfileSchema,
    VolumeProfileGridHeight,
    VolumeProfilePayloadEstimate,
    VolumeProfilePayloadDecoded,
    VolumeProfileBase64Decode,
    Count
};

struct DropLogBucket {
    QElapsedTimer timer;
    bool started = false;
};

bool shouldLogDrop(DropReason reason) {
    static std::array<DropLogBucket, static_cast<size_t>(DropReason::Count)> buckets;
    auto& bucket = buckets[static_cast<size_t>(reason)];
    if (!bucket.started) {
        bucket.timer.start();
        bucket.started = true;
        return true;
    }
    if (bucket.timer.elapsed() >= kSchemaLogThrottleMs) {
        bucket.timer.restart();
        return true;
    }
    return false;
}

void logDroppedMessage(DropReason reason, const QString& msg) {
    if (shouldLogDrop(reason)) {
        sLog_Warning(msg);
    }
}

bool validateFamilySchema(const nlohmann::json& msg,
                          const char* family,
                          int supportedVersion,
                          DropReason reason) {
    const int ver = protocol::validation::extractSchemaVersion(msg);
    if (ver < 0) {
        logDroppedMessage(reason,
                          QString("Dropping %1 message: missing or invalid schema_version")
                              .arg(QString::fromUtf8(family)));
        return false;
    }
    if (ver != supportedVersion) {
        logDroppedMessage(reason,
                          QString("Dropping %1 message: unsupported schema_version=%2 (supported=%3)")
                              .arg(QString::fromUtf8(family))
                              .arg(ver)
                              .arg(supportedVersion));
        return false;
    }
    return true;
}

bool validateGridHeight(const char* messageType, int gridHeight, DropReason reason) {
    if (!protocol::validation::isGridHeightValid(gridHeight)) {
        logDroppedMessage(reason,
                          QString("Dropping %1: grid_height=%2 out of bounds (max=%3)")
                              .arg(QString::fromUtf8(messageType))
                              .arg(gridHeight)
                              .arg(protocol::SentinelProtocol::kMaxGridHeight));
        return false;
    }
    return true;
}

size_t estimateBase64DecodedBytes(const std::string& encoded) {
    return protocol::validation::estimateBase64DecodedBytes(encoded);
}

bool validateEncodedPayloadEstimate(const char* messageType,
                                    const char* fieldName,
                                    const std::string& encoded,
                                    DropReason reason) {
    const size_t estimated = estimateBase64DecodedBytes(encoded);
    if (!protocol::validation::isPayloadSizeValid(estimated)) {
        logDroppedMessage(reason,
                          QString("Dropping %1: %2 estimated decode bytes=%3 exceeds max=%4")
                              .arg(QString::fromUtf8(messageType))
                              .arg(QString::fromUtf8(fieldName))
                              .arg(static_cast<qulonglong>(estimated))
                              .arg(protocol::SentinelProtocol::kMaxPayloadBytes));
        return false;
    }
    return true;
}

bool decodeBase64WithGuardrails(const char* messageType,
                                const char* fieldName,
                                const std::string& encoded,
                                DropReason estimateReason,
                                DropReason decodeReason,
                                DropReason payloadReason,
                                QByteArray& out) {
    out.clear();
    if (encoded.empty()) {
        return true;
    }
    if (!validateEncodedPayloadEstimate(messageType, fieldName, encoded, estimateReason)) {
        return false;
    }
    const QByteArray decoded = QByteArray::fromBase64(QByteArray::fromStdString(encoded),
                                                      QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty()) {
        logDroppedMessage(decodeReason,
                          QString("Dropping %1: %2 failed base64 decode")
                              .arg(QString::fromUtf8(messageType))
                              .arg(QString::fromUtf8(fieldName)));
        return false;
    }
    if (decoded.size() > protocol::SentinelProtocol::kMaxPayloadBytes) {
        logDroppedMessage(payloadReason,
                          QString("Dropping %1: %2 decoded bytes=%3 exceeds max=%4")
                              .arg(QString::fromUtf8(messageType))
                              .arg(QString::fromUtf8(fieldName))
                              .arg(decoded.size())
                              .arg(protocol::SentinelProtocol::kMaxPayloadBytes));
        return false;
    }
    out = decoded;
    return true;
}

} // namespace

SentinelStreamClient::SentinelStreamClient(const std::string& host, const std::string& port,
                                           const std::string& caFile, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_port(port)
{
    if (!caFile.empty()) {
        boost::system::error_code sslEc;
        m_sslCtx.load_verify_file(caFile, sslEc);
        if (sslEc) {
            sLog_Warning("SentinelStreamClient: could not load CA cert '"
                         << caFile << "': " << sslEc.message()
                         << " - TLS peer verification disabled (dev mode)");
            m_sslCtx.set_verify_mode(ssl::verify_none);
        } else {
            m_sslCtx.set_verify_mode(ssl::verify_peer);
        }
    } else {
        sLog_Warning("SentinelStreamClient: no ca_file configured - TLS peer verification disabled");
        m_sslCtx.set_verify_mode(ssl::verify_none);
    }

    qRegisterMetaType<BookLevelUpdate>("BookLevelUpdate");
    qRegisterMetaType<std::vector<BookLevelUpdate>>("BookLevelUpdateVector");
    qRegisterMetaType<OrderBookLevel>("OrderBookLevel");
    qRegisterMetaType<std::vector<OrderBookLevel>>("OrderBookLevelVector");
    qRegisterMetaType<HeatmapHistoryColumn>("HeatmapHistoryColumn");
    qRegisterMetaType<QVector<HeatmapHistoryColumn>>("QVector<HeatmapHistoryColumn>");
    qRegisterMetaType<HeatmapSlice>("HeatmapSlice");
    qRegisterMetaType<FootprintSlice>("FootprintSlice");
    qRegisterMetaType<TpoSlice>("TpoSlice");
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
            
            boost::beast::get_lowest_layer(m_ws).async_connect(
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

void SentinelStreamClient::requestFootprintHistory(const std::string& symbol,
                                                   int64_t timeframeMs,
                                                   int64_t endTimeMs,
                                                   int count) {
    if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
        return;
    }
    nlohmann::json msg = {
        {"type", "footprint_history_request"},
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

void SentinelStreamClient::requestTpoHistory(const std::string& symbol,
                                             int64_t timeframeMs,
                                             int64_t endTimeMs,
                                             int count) {
    if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
        return;
    }
    nlohmann::json msg = {
        {"type", "tpo_history_request"},
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

void SentinelStreamClient::requestScreenerData(const std::string& asset,
                                               int limit,
                                               double minVolume) {
    nlohmann::json msg = {
        {"type",       "screener_request"},
        {"asset",      asset.empty() ? "crypto" : asset},
        {"limit",      limit > 0 ? limit : 50},
        {"min_volume", minVolume},
    };
    std::string str = msg.dump();
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::sendTradeCommand(const trading::TradeCommand& command) {
    nlohmann::json msg = {
        {"type", "trade_command"},
        {"command_id", command.commandId},
        {"action", trading::toString(command.action)},
        {"symbol", command.symbol},
        {"side", trading::toString(command.side)},
        {"order_type", trading::toString(command.orderType)},
        {"qty", command.qty},
        {"timestamp", command.timestamp}
    };
    msg["price"] = command.hasPrice ? nlohmann::json(command.price) : nlohmann::json(nullptr);
    if (!command.targetOrderId.empty()) {
        msg["order_id"] = command.targetOrderId;
    }

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
    
    m_ws.next_layer().async_handshake(
        ssl::stream_base::client,
        [this](auto ec) { onSslHandshake(ec); });
}

void SentinelStreamClient::onSslHandshake(boost::beast::error_code ec) {
    if (ec) {
        sLog_Error("SSL handshake failed: " << ec.message());
        emit errorOccurred(QString::fromStdString("SSL: " + ec.message()));
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
        const auto msg = nlohmann::json::parse(msgStr);
        const std::string typeStr = msg.value("type", "unknown");
        const auto type = protocol::fromString(typeStr);

        switch (type) {
            case protocol::MessageType::ServerConfig:
                handleServerConfigMessage(msg);
                return;
            case protocol::MessageType::Snapshot:
                handleSnapshotMessage(msg);
                return;
            case protocol::MessageType::HeatmapSlice:
                handleHeatmapSliceMessage(msg);
                return;
            case protocol::MessageType::HeatmapHistoryChunk:
                handleHeatmapHistoryChunkMessage(msg);
                return;
            case protocol::MessageType::CandleHistoryChunk:
                handleCandleHistoryChunkMessage(msg);
                return;
            case protocol::MessageType::CandleBarUpdate:
            case protocol::MessageType::CandleBarClosed:
                handleCandleBarMessage(type, msg);
                return;
            case protocol::MessageType::FootprintConfig:
                handleFootprintConfigMessage(msg);
                return;
            case protocol::MessageType::FootprintSlice:
                handleFootprintSliceMessage(msg);
                return;
            case protocol::MessageType::FootprintHistoryChunk:
                handleFootprintHistoryChunkMessage(msg);
                return;
            case protocol::MessageType::TpoSlice:
                handleTpoSliceMessage(msg);
                return;
            case protocol::MessageType::TpoHistoryChunk:
                handleTpoHistoryChunkMessage(msg);
                return;
            case protocol::MessageType::OrderUpdate:
                handleOrderUpdateMessage(msg);
                return;
            case protocol::MessageType::PositionUpdate:
                handlePositionUpdateMessage(msg);
                return;
            case protocol::MessageType::ScreenerUpdate:
                handleScreenerUpdateMessage(msg);
                return;
            case protocol::MessageType::VolumeProfileSlice:
                handleVolumeProfileSliceMessage(msg);
                return;
            case protocol::MessageType::Unknown:
                break;
            default:
                break;
        }

        if (typeStr == "l2update") {
            handleL2UpdateMessage(msg);
            return;
        }
        if (typeStr == "trade") {
            handleTradeMessage(msg);
            return;
        }

    } catch (const std::exception& e) {
        sLog_Error("Message parse error: " << e.what());
    }
}

void SentinelStreamClient::handleServerConfigMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "server_config",
                              protocol::SentinelProtocol::kServerConfigSchemaVersion,
                              DropReason::ServerSchema)) {
        return;
    }
    emit serverConfigReceived(protocol::clientparse::parseServerConfig(msg));
}

void SentinelStreamClient::handleSnapshotMessage(const nlohmann::json& msg) {
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const auto bids = protocol::clientparse::parseOrderBookLevels(msg.value("bids", nlohmann::json::array()));
    const auto asks = protocol::clientparse::parseOrderBookLevels(msg.value("asks", nlohmann::json::array()));
    emit snapshotReceived(QString::fromStdString(symbol), bids, asks);
}

void SentinelStreamClient::handleL2UpdateMessage(const nlohmann::json& msg) {
    const std::string symbol = msg.value("product_id", "");
    if (symbol.empty() || !msg.contains("deltas")) {
        return;
    }
    const auto updates = protocol::clientparse::parseL2Updates(msg["deltas"]);
    emit l2UpdateReceived(QString::fromStdString(symbol), updates);
}

void SentinelStreamClient::handleTradeMessage(const nlohmann::json& msg) {
    Trade t;
    t.product_id = msg.value("product_id", "");
    t.price = msg.value("price", 0.0);
    t.size = msg.value("size", 0.0);
    const std::string side = msg.value("side", "");
    t.side = (side == "buy") ? AggressorSide::Buy : AggressorSide::Sell;
    emit tradeReceived(t);
}

void SentinelStreamClient::handleHeatmapSliceMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "heatmap",
                              protocol::SentinelProtocol::kHeatmapSchemaVersion,
                              DropReason::HeatmapSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }

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

    if (!validateGridHeight("heatmap_slice", gridHeight, DropReason::HeatmapGridHeight)) {
        return;
    }

    QByteArray column;
    if (!decodeBase64WithGuardrails("heatmap_slice",
                                    "column",
                                    encoded,
                                    DropReason::HeatmapPayloadEstimate,
                                    DropReason::HeatmapBase64Decode,
                                    DropReason::HeatmapPayloadDecoded,
                                    column)) {
        return;
    }
    QByteArray liquidityColumn;
    if (!decodeBase64WithGuardrails("heatmap_slice",
                                    "liquidity_column",
                                    liquidityEncoded,
                                    DropReason::HeatmapPayloadEstimate,
                                    DropReason::HeatmapBase64Decode,
                                    DropReason::HeatmapPayloadDecoded,
                                    liquidityColumn)) {
        return;
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
}

void SentinelStreamClient::handleHeatmapHistoryChunkMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "heatmap",
                              protocol::SentinelProtocol::kHeatmapSchemaVersion,
                              DropReason::HeatmapSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const std::string encoding = msg.value("encoding", "base64");
    const std::string liquidityEncoding = msg.value("liquidity_encoding", "base64");
    const auto columns = msg.value("columns", nlohmann::json::array());

    if (!validateGridHeight("heatmap_history_chunk", gridHeight, DropReason::HeatmapHistoryGridHeight)) {
        return;
    }

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
                if (!decodeBase64WithGuardrails("heatmap_history_chunk",
                                                "column",
                                                encoded,
                                                DropReason::HeatmapHistoryPayloadEstimate,
                                                DropReason::HeatmapHistoryBase64Decode,
                                                DropReason::HeatmapHistoryPayloadDecoded,
                                                col.intensity)) {
                    return;
                }
            }
            const std::string liqEncoded = item.value("liquidity_column", "");
            if (!liqEncoded.empty() && liquidityEncoding == "base64") {
                if (!decodeBase64WithGuardrails("heatmap_history_chunk",
                                                "liquidity_column",
                                                liqEncoded,
                                                DropReason::HeatmapHistoryPayloadEstimate,
                                                DropReason::HeatmapHistoryBase64Decode,
                                                DropReason::HeatmapHistoryPayloadDecoded,
                                                col.liquidity)) {
                    return;
                }
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

    emit heatmapHistoryReceived(QString::fromStdString(symbol), timeframeMs, gridWidth, gridHeight, out);
}

void SentinelStreamClient::handleCandleHistoryChunkMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "candle",
                              protocol::SentinelProtocol::kCandleSchemaVersion,
                              DropReason::CandleSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeSec = msg.value("timeframe_sec", static_cast<int64_t>(0));
    const int64_t startTimeSec = msg.value("start_time_sec", static_cast<int64_t>(0));
    const int64_t endTimeSec = msg.value("end_time_sec", static_cast<int64_t>(0));
    const auto candles = msg.value("candles", nlohmann::json::array());

    QVector<CandleBar> out;
    if (candles.is_array()) {
        out.reserve(static_cast<int>(candles.size()));
        for (const auto& item : candles) {
            out.push_back(protocol::clientparse::parseCandleBar(item));
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

    emit candleHistoryReceived(QString::fromStdString(symbol), timeframeSec, startTimeSec, endTimeSec, out);
}

void SentinelStreamClient::handleCandleBarMessage(protocol::MessageType type, const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "candle",
                              protocol::SentinelProtocol::kCandleSchemaVersion,
                              DropReason::CandleSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeSec = msg.value("timeframe_sec", static_cast<int64_t>(0));
    const int64_t bucketStartMs = msg.value("bucket_start_ms", static_cast<int64_t>(0));
    const int64_t seq = msg.value("seq", static_cast<int64_t>(0));
    const auto item = msg.value("candle", nlohmann::json::object());
    const CandleBar bar = protocol::clientparse::parseCandleBar(item);

    const auto symbolQ = QString::fromStdString(symbol);
    if (type == protocol::MessageType::CandleBarClosed) {
        emit candleBarClosedReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar);
    } else {
        emit candleBarUpdateReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar);
    }
}

void SentinelStreamClient::handleFootprintConfigMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "footprint",
                              protocol::SentinelProtocol::kFootprintSchemaVersion,
                              DropReason::FootprintSchema)) {
        return;
    }
}

void SentinelStreamClient::handleFootprintSliceMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "footprint",
                              protocol::SentinelProtocol::kFootprintSchemaVersion,
                              DropReason::FootprintSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t startMs = msg.value("time_start", static_cast<int64_t>(0));
    const int64_t endMs = msg.value("time_end", static_cast<int64_t>(0));
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const double minPrice = msg.value("min_price", 0.0);
    const double maxPrice = msg.value("max_price", 0.0);
    const double tickSize = msg.value("tick_size", 0.0);
    const double quantScale = msg.value("quant_scale", 1.0);
    const std::string format = msg.value("format", "q16_delta");
    const std::string encoded = msg.value("delta_levels_q16", "");

    if (!validateGridHeight("footprint_slice", gridHeight, DropReason::FootprintGridHeight)) {
        return;
    }
    if (startMs <= 0 || endMs <= startMs || timeframeMs <= 0 ||
        tickSize <= 0.0 || maxPrice <= minPrice || quantScale <= 0.0) {
        logDroppedMessage(DropReason::FootprintSliceMeta,
                          QString("Dropping footprint_slice: invalid metadata (start=%1 end=%2 tf=%3 tick=%4 range=[%5,%6] quant=%7)")
                              .arg(startMs)
                              .arg(endMs)
                              .arg(timeframeMs)
                              .arg(tickSize, 0, 'g', 8)
                              .arg(minPrice, 0, 'g', 8)
                              .arg(maxPrice, 0, 'g', 8)
                              .arg(quantScale, 0, 'g', 8));
        return;
    }

    QByteArray deltaLevelsQ16;
    if (!decodeBase64WithGuardrails("footprint_slice",
                                    "delta_levels_q16",
                                    encoded,
                                    DropReason::FootprintPayloadEstimate,
                                    DropReason::FootprintBase64Decode,
                                    DropReason::FootprintPayloadDecoded,
                                    deltaLevelsQ16)) {
        return;
    }
    const int expectedBytes = gridHeight * static_cast<int>(sizeof(int16_t));
    if (deltaLevelsQ16.size() != expectedBytes) {
        logDroppedMessage(DropReason::FootprintPayloadShape,
                          QString("Dropping footprint_slice: delta_levels_q16 bytes=%1 expected=%2")
                              .arg(deltaLevelsQ16.size())
                              .arg(expectedBytes));
        return;
    }

    FootprintSlice slice;
    slice.symbol = QString::fromStdString(symbol);
    slice.bucketStartMs = startMs;
    slice.bucketEndMs = endMs;
    slice.timeframeMs = timeframeMs;
    slice.gridWidth = gridWidth;
    slice.gridHeight = gridHeight;
    slice.minPrice = minPrice;
    slice.maxPrice = maxPrice;
    slice.tickSize = tickSize;
    slice.quantScale = quantScale;
    slice.format = QString::fromStdString(format);
    slice.deltaLevelsQ16 = std::move(deltaLevelsQ16);
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Footprint recv: symbol=%1 t=[%2..%3] tfMs=%4 grid=%5x%6 bytes=%7")
                       .arg(slice.symbol)
                       .arg(slice.bucketStartMs)
                       .arg(slice.bucketEndMs)
                       .arg(slice.timeframeMs)
                       .arg(slice.gridWidth)
                       .arg(slice.gridHeight)
                       .arg(slice.deltaLevelsQ16.size()));
    }
    emit footprintSliceReceived(slice);
}

void SentinelStreamClient::handleFootprintHistoryChunkMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "footprint",
                              protocol::SentinelProtocol::kFootprintSchemaVersion,
                              DropReason::FootprintSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const std::string encoding = msg.value("encoding", "base64");
    const auto columns = msg.value("columns", nlohmann::json::array());

    if (!validateGridHeight("footprint_history_chunk", gridHeight, DropReason::FootprintGridHeight)) {
        return;
    }

    if (!columns.is_array()) {
        return;
    }

    int emitted = 0;
    for (const auto& item : columns) {
        const int64_t startMs = item.value("time_start", static_cast<int64_t>(0));
        const int64_t endMs = item.value("time_end", static_cast<int64_t>(0));
        const double minPrice = item.value("min_price", 0.0);
        const double maxPrice = item.value("max_price", 0.0);
        const double tickSize = item.value("tick_size", 0.0);
        const double quantScale = item.value("quant_scale", 1.0);
        const std::string format = item.value("format", "q16_delta");
        const std::string encoded = item.value("delta_levels_q16", "");

        if (startMs <= 0 || endMs <= startMs || timeframeMs <= 0 ||
            tickSize <= 0.0 || maxPrice <= minPrice || quantScale <= 0.0) {
            continue;
        }

        QByteArray deltaLevelsQ16;
        if (!encoded.empty() && encoding == "base64") {
            if (!decodeBase64WithGuardrails("footprint_history_chunk",
                                            "delta_levels_q16",
                                            encoded,
                                            DropReason::FootprintPayloadEstimate,
                                            DropReason::FootprintBase64Decode,
                                            DropReason::FootprintPayloadDecoded,
                                            deltaLevelsQ16)) {
                return;
            }
        }
        const int expectedBytes = gridHeight * static_cast<int>(sizeof(int16_t));
        if (deltaLevelsQ16.size() != expectedBytes) {
            continue;
        }

        FootprintSlice slice;
        slice.symbol = QString::fromStdString(symbol);
        slice.bucketStartMs = startMs;
        slice.bucketEndMs = endMs;
        slice.timeframeMs = timeframeMs;
        slice.gridWidth = gridWidth;
        slice.gridHeight = gridHeight;
        slice.minPrice = minPrice;
        slice.maxPrice = maxPrice;
        slice.tickSize = tickSize;
        slice.quantScale = quantScale;
        slice.format = QString::fromStdString(format);
        slice.deltaLevelsQ16 = std::move(deltaLevelsQ16);
        emit footprintSliceReceived(slice);
        ++emitted;
    }

    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Footprint history recv: symbol=%1 tfMs=%2 count=%3")
                       .arg(QString::fromStdString(symbol))
                       .arg(timeframeMs)
                       .arg(emitted));
    }
}

void SentinelStreamClient::handleOrderUpdateMessage(const nlohmann::json& msg) {
    trading::OrderUpdate update;
    update.orderId = msg.value("order_id", "");
    update.symbol = msg.value("symbol", "");
    update.status = trading::orderStatusFromString(msg.value("status", "REJECTED"));
    update.side = trading::orderSideFromString(msg.value("side", "UNKNOWN"));
    update.qty = msg.value("qty", 0.0);
    update.filledQty = msg.value("filled_qty", 0.0);
    update.remainingQty = msg.value("remaining_qty", 0.0);
    update.avgPrice = msg.value("avg_price", 0.0);
    if (!update.orderId.empty()) {
        emit orderUpdated(update);
    }
}

void SentinelStreamClient::handlePositionUpdateMessage(const nlohmann::json& msg) {
    trading::PositionUpdate update;
    update.symbol = msg.value("symbol", "");
    update.positionQty = msg.value("position_qty", 0.0);
    update.avgPrice = msg.value("avg_price", 0.0);
    update.unrealizedPnl = msg.value("unrealized_pnl", 0.0);
    if (!update.symbol.empty()) {
        emit positionUpdated(update);
    }
}

void SentinelStreamClient::handleScreenerUpdateMessage(const nlohmann::json& msg) {
    const std::string asset = msg.value("asset", "crypto");
    const int rowCount = msg.value("row_count", 0);
    const auto& rows = msg.contains("rows") ? msg["rows"] : nlohmann::json::array();
    const QByteArray rowsJson = QByteArray::fromStdString(rows.dump());
    emit screenerUpdateReceived(QString::fromStdString(asset), rowCount, rowsJson);
}

void SentinelStreamClient::handleTpoSliceMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "tpo",
                              protocol::SentinelProtocol::kTpoSchemaVersion,
                              DropReason::TpoSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t startMs = msg.value("time_start", static_cast<int64_t>(0));
    const int64_t endMs = msg.value("time_end", static_cast<int64_t>(0));
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const double minPrice = msg.value("min_price", 0.0);
    const double maxPrice = msg.value("max_price", 0.0);
    const double tickSize = msg.value("tick_size", 0.0);
    const std::string format = msg.value("format", "tpo_ascii");
    const std::string encoded = msg.value("letters", "");

    if (!validateGridHeight("tpo_slice", gridHeight, DropReason::TpoGridHeight)) {
        return;
    }
    if (startMs <= 0 || endMs <= startMs || timeframeMs <= 0 || maxPrice <= minPrice || tickSize <= 0.0) {
        return;
    }

    QByteArray letters;
    if (!decodeBase64WithGuardrails("tpo_slice",
                                    "letters",
                                    encoded,
                                    DropReason::TpoPayloadEstimate,
                                    DropReason::TpoBase64Decode,
                                    DropReason::TpoPayloadDecoded,
                                    letters)) {
        return;
    }
    if (letters.size() != gridHeight) {
        return;
    }

    TpoSlice slice;
    slice.symbol = QString::fromStdString(symbol);
    slice.bucketStartMs = startMs;
    slice.bucketEndMs = endMs;
    slice.timeframeMs = timeframeMs;
    slice.gridWidth = gridWidth;
    slice.gridHeight = gridHeight;
    slice.minPrice = minPrice;
    slice.maxPrice = maxPrice;
    slice.tickSize = tickSize;
    slice.format = QString::fromStdString(format);
    slice.letters = std::move(letters);
    emit tpoSliceReceived(slice);
}

void SentinelStreamClient::handleTpoHistoryChunkMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "tpo",
                              protocol::SentinelProtocol::kTpoSchemaVersion,
                              DropReason::TpoSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const std::string encoding = msg.value("encoding", "base64");
    const auto columns = msg.value("columns", nlohmann::json::array());

    if (!validateGridHeight("tpo_history_chunk", gridHeight, DropReason::TpoGridHeight)) {
        return;
    }
    if (!columns.is_array()) {
        return;
    }

    for (const auto& item : columns) {
        const int64_t startMs = item.value("time_start", static_cast<int64_t>(0));
        const int64_t endMs = item.value("time_end", static_cast<int64_t>(0));
        const double minPrice = item.value("min_price", 0.0);
        const double maxPrice = item.value("max_price", 0.0);
        const double tickSize = item.value("tick_size", 0.0);
        const std::string format = item.value("format", "tpo_ascii");
        const std::string encoded = item.value("letters", "");
        if (startMs <= 0 || endMs <= startMs || timeframeMs <= 0 || maxPrice <= minPrice || tickSize <= 0.0) {
            continue;
        }
        QByteArray letters;
        if (!encoded.empty() && encoding == "base64") {
            if (!decodeBase64WithGuardrails("tpo_history_chunk",
                                            "letters",
                                            encoded,
                                            DropReason::TpoPayloadEstimate,
                                            DropReason::TpoBase64Decode,
                                            DropReason::TpoPayloadDecoded,
                                            letters)) {
                return;
            }
        }
        if (letters.size() != gridHeight) {
            continue;
        }
        TpoSlice slice;
        slice.symbol = QString::fromStdString(symbol);
        slice.bucketStartMs = startMs;
        slice.bucketEndMs = endMs;
        slice.timeframeMs = timeframeMs;
        slice.gridWidth = gridWidth;
        slice.gridHeight = gridHeight;
        slice.minPrice = minPrice;
        slice.maxPrice = maxPrice;
        slice.tickSize = tickSize;
        slice.format = QString::fromStdString(format);
        slice.letters = std::move(letters);
        emit tpoSliceReceived(slice);
    }
}

void SentinelStreamClient::handleVolumeProfileSliceMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "volume_profile",
                              protocol::SentinelProtocol::kVolumeProfileSchemaVersion,
                              DropReason::VolumeProfileSchema)) {
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t sessionStartMs = msg.value("session_start_ms", static_cast<int64_t>(0));
    const int64_t sessionEndMs   = msg.value("session_end_ms",   static_cast<int64_t>(0));
    const int     sessionType    = msg.value("session_type",     4);
    const double  minPrice       = msg.value("min_price",        0.0);
    const double  maxPrice       = msg.value("max_price",        0.0);
    const double  tickSize       = msg.value("tick_size",        0.0);
    const int     gridHeight     = msg.value("grid_height",      0);
    const double  totalVolume    = msg.value("total_volume",     0.0);
    const double  pocPrice       = msg.value("poc_price",        0.0);
    const double  vahPrice       = msg.value("vah_price",        0.0);
    const double  valPrice       = msg.value("val_price",        0.0);
    const std::string encoded    = msg.value("volume_bins",      "");

    if (!validateGridHeight("volume_profile_slice", gridHeight, DropReason::VolumeProfileGridHeight)) {
        return;
    }
    if (sessionStartMs <= 0 || sessionEndMs <= sessionStartMs || maxPrice <= minPrice || tickSize <= 0.0) {
        return;
    }

    QByteArray bins;
    if (!encoded.empty()) {
        if (!decodeBase64WithGuardrails("volume_profile_slice",
                                        "volume_bins",
                                        encoded,
                                        DropReason::VolumeProfilePayloadEstimate,
                                        DropReason::VolumeProfileBase64Decode,
                                        DropReason::VolumeProfilePayloadDecoded,
                                        bins)) {
            return;
        }
    }

    const int expectedBytes = gridHeight * static_cast<int>(sizeof(float));
    if (!bins.isEmpty() && bins.size() != expectedBytes) {
        logDroppedMessage(DropReason::VolumeProfilePayloadDecoded,
                          QString("Dropping volume_profile_slice: bins bytes=%1 expected=%2")
                              .arg(bins.size())
                              .arg(expectedBytes));
        return;
    }

    VolumeProfileSlice slice;
    slice.symbol         = QString::fromStdString(symbol);
    slice.sessionStartMs = sessionStartMs;
    slice.sessionEndMs   = sessionEndMs;
    slice.sessionType    = sessionType;
    slice.minPrice       = minPrice;
    slice.maxPrice       = maxPrice;
    slice.tickSize       = tickSize;
    slice.gridHeight     = gridHeight;
    slice.totalVolume    = totalVolume;
    slice.pocPrice       = pocPrice;
    slice.vahPrice       = vahPrice;
    slice.valPrice       = valPrice;
    slice.volumeBinsF32  = std::move(bins);
    emit volumeProfileSliceReceived(slice);
}
