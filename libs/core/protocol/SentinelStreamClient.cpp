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
    VolumeProfileSliceMeta,
    TpoSliceMeta,
    TpoPayloadShape,
    UnknownType,
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
    qRegisterMetaType<RecordingHistoryPage>("RecordingHistoryPage");
    qRegisterMetaType<HeatmapSlice>("HeatmapSlice");
    qRegisterMetaType<FootprintSlice>("FootprintSlice");
    qRegisterMetaType<TpoSlice>("TpoSlice");
    qRegisterMetaType<CandleBar>("CandleBar");
    qRegisterMetaType<QVector<CandleBar>>("QVector<CandleBar>");
    qRegisterMetaType<ServerConfig>("ServerConfig");
    qRegisterMetaType<HeatmapChunkPtr>("SentinelStreamClient::HeatmapChunkPtr");
    qRegisterMetaType<HeatmapChunkError>("SentinelStreamClient::HeatmapChunkError");
    qRegisterMetaType<protocol::chunkwire::Availability>("protocol::chunkwire::Availability");
    m_decodePool = std::make_unique<net::thread_pool>(1);
}

SentinelStreamClient::~SentinelStreamClient() {
    disconnectFromServer();
    // Queued decodes are abandoned; a running one finishes before members die.
    m_decodePool->stop();
    m_decodePool->join();
}

void SentinelStreamClient::connectToServer() {
    if (m_running) return;
    m_ws = std::make_unique<WebSocket>(m_strand, m_sslCtx);
    {
        std::lock_guard lock(m_chunkOrderMutex);
        ++m_connectionEpoch;
        m_acceptChunkFrames = true;
        m_decodeRefusals = 0;
        m_chunkOrder.clear();
    }
    m_ioc.restart();
    m_isConnected = false;
    Q_ASSERT(!m_writeInFlight); // disconnect drains completions before queue reuse
    m_writeQueue.clear();

    m_running = true;
    m_work = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(m_ioc.get_executor());
    sLog_Data("SentinelStreamClient connecting: host=" << m_host << " port=" << m_port);

    m_thread = std::thread([this] {
        sentinel::logging::setCurrentThreadName("stream-client");
        try {
            tcp::resolver resolver(m_ioc);
            auto const results = resolver.resolve(m_host, m_port);
            
            boost::beast::get_lowest_layer(*m_ws).async_connect(
                results,
                [this](auto ec, tcp::endpoint ep) { onConnect(ec, ep); }
            );
            
            m_ioc.run();
        } catch (const std::exception& e) {
            sLog_Error("Client thread exception: host=" << m_host << " port=" << m_port
                       << " error=" << e.what());
            emit errorOccurred(QString::fromStdString(e.what()));
        }
    });
}

void SentinelStreamClient::disconnectFromServer() {
    {
        std::lock_guard lock(m_chunkOrderMutex);
        ++m_connectionEpoch;
        m_acceptChunkFrames = false;
        m_chunkOrder.clear();
    }
    if (m_running) {
        sLog_Data("SentinelStreamClient disconnecting: host=" << m_host << " port=" << m_port
                  << " connected=" << m_isConnected.load());
    }
    m_running = false;
    m_isConnected = false;
    if (m_thread.joinable()) {
        // stop() alone strands callbacks that still borrow queue.front(). Close
        // on the strand and drain them before clearing buffers or restarting.
        net::post(m_strand, [this] {
            boost::beast::error_code ignored;
            boost::beast::get_lowest_layer(*m_ws).socket().close(ignored);
            if (m_work) m_work->reset();
        });
        m_thread.join();
        // Also drain if the worker exited via its exception handler before run().
        m_ioc.restart();
        m_ioc.run();
    }
    m_writeQueue.clear();
    m_buffer.consume(m_buffer.size());
    Q_ASSERT(!m_writeInFlight);
}

void SentinelStreamClient::subscribe(const std::string& symbol) {
    {
        std::lock_guard lock(m_bookDeliveryMutex);
        ++m_bookDeliveryGenerations[symbol];
    }
    sLog_Data("Client subscribe: symbol=" << symbol);
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

quint64 SentinelStreamClient::bookDeliveryGeneration(const std::string& symbol) const {
    std::lock_guard lock(m_bookDeliveryMutex);
    const auto it = m_bookDeliveryGenerations.find(symbol);
    return it == m_bookDeliveryGenerations.end() ? 0 : it->second;
}

void SentinelStreamClient::unsubscribe(const std::string& symbol) {
    {
        std::lock_guard lock(m_bookDeliveryMutex);
        ++m_bookDeliveryGenerations[symbol];
    }
    sLog_Data("Client unsubscribe: symbol=" << symbol);
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
        sLog_Warning("Heatmap history request not sent: invalid args symbol=" << symbol
                     << " tfMs=" << timeframeMs << " count=" << count);
        return;
    }
    const int boundedCount = std::min(count, protocol::SentinelProtocol::kMaxHeatmapHistoryColumns);
    sLog_Data("Heatmap history request: symbol=" << symbol << " tfMs=" << timeframeMs
              << " end=" << endTimeMs << " count=" << boundedCount);
    nlohmann::json msg = {
        {"type", "heatmap_history_request"},
        {"symbol", symbol},
        {"timeframe_ms", timeframeMs},
        {"end_time", endTimeMs},
        {"count", boundedCount}
    };

    std::string str = msg.dump();
    net::post(m_strand, [this, payload = std::move(str)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::registerRecordingView(const recording::LiveView& view) {
    auto msg = protocol::recordingwire::viewMessage(view);
    if (!protocol::recordingwire::parseView(msg)) return;
    net::post(m_strand, [this, payload = msg.dump()]() mutable {
        if (!m_isConnected) return; // reconnect registers the newly confirmed band
        m_writeQueue.push_back(std::move(payload));
        if (m_writeQueue.size() == 1) doWrite();
    });
}

void SentinelStreamClient::releaseRecordingView(const std::string& symbol) {
    if (symbol.empty() || symbol.size() > 128) return;
    net::post(m_strand, [this, payload = protocol::recordingwire::unviewMessage(symbol).dump()]() mutable {
        if (!m_isConnected) return; // a new connection has no view registered
        m_writeQueue.push_back(std::move(payload));
        if (m_writeQueue.size() == 1) doWrite();
    });
}

void SentinelStreamClient::requestRecordingHeatmapHistory(const protocol::recordingwire::Request& request) {
    nlohmann::json msg = {{"type", "heatmap_history_request"}, {"source", "recording"},
                          {"symbol", request.symbol}, {"timeframe_ms", request.timeframeMs},
                          {"end_time", request.endTimeMs}, {"count", request.count},
                          {"price_min", request.priceMin}, {"price_max", request.priceMax},
                          {"rows", request.rows}, {"request_id", request.requestId},
                          {"band_generation", request.bandGeneration}};
    if (request.displayTick) msg["display_tick"] = *request.displayTick;
    const auto parsed = protocol::recordingwire::parseRequest(msg);
    if (!parsed) {
        sLog_Warning("Recording history request not sent: invalid args symbol=" << request.symbol);
        return;
    }
    msg["count"] = parsed->count;
    std::string payload = msg.dump();
    net::post(m_strand, [this, payload = std::move(payload)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) doWrite();
    });
}

quint64 SentinelStreamClient::requestHeatmapChunks(const std::string& symbol, const std::string& source,
                                                  int64_t levelMs, const std::vector<int64_t>& starts,
                                                  const std::vector<uint64_t>& haveHash) {
    protocol::chunkwire::Request q;
    q.req = ++m_nextChunkRequestId;
    q.symbol = symbol; q.source = source; q.levelMs = levelMs; q.starts = starts;
    q.haveHash = haveHash.empty() ? std::vector<uint64_t>(starts.size(), 0) : haveHash;
    sLog_Probe("chunks.request", "req=" << q.req << " symbol=" << symbol << " source=" << source
               << " level=" << levelMs << " starts=" << starts.size());
    net::post(m_strand, [this, payload = protocol::chunkwire::buildRequest(q).dump()]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) doWrite();
    });
    return q.req;
}

quint64 SentinelStreamClient::subscribeHeatmapLive(const std::string& symbol,
    const std::vector<std::string>& sources, int64_t sinceMs) {
    const auto id = ++m_nextChunkRequestId;
    recording::RawTailView view{symbol, sources, id, sinceMs};
    net::post(m_strand, [this, payload = protocol::chunkwire::buildLiveSubscribe(view).dump()]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) doWrite();
    });
    return id;
}
void SentinelStreamClient::unsubscribeHeatmapLive(const std::string& symbol) {
    const nlohmann::json request = {{"type", "heatmap_live_unsubscribe"}, {"symbol", symbol}};
    net::post(m_strand, [this, payload = request.dump()]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) doWrite();
    });
}

namespace {
quint64 peekEnvelopeRequestId(const std::vector<uint8_t>& frame) {
    static constexpr uint8_t kMagic[4] = {'S', 'H', 'E', '1'};
    if (frame.size() < 14 || !std::equal(kMagic, kMagic + 4, frame.begin())) return 0;
    quint64 id = 0;
    for (int i = 0; i < 8; ++i) id |= quint64(frame[6 + i]) << (8 * i);
    return id;
}
} // namespace

void SentinelStreamClient::handleBinaryMessage(std::shared_ptr<std::vector<uint8_t>> frame) {
    std::lock_guard lock(m_chunkOrderMutex);
    if (!m_acceptChunkFrames) return;
    const size_t bytes = frame->size();
    // Refusals run inline, with at most one notification per reason until the
    // decoder makes progress. A flood must not create a second unbounded queue
    // of rejection tasks or queued Qt signals while the decoder is stalled.
    const auto refuse = [&](unsigned reason, const char* code, const char* message) {
        if (m_decodeRefusals & reason) return;
        m_decodeRefusals |= reason;
        sLog_Warning("Heatmap chunk refused: bytes=" << bytes << " reason=" << message);
        emit heatmapChunkFailed({peekEnvelopeRequestId(*frame), {}, QString::fromLatin1(code),
                                 QString::fromLatin1(message)});
    };
    if (bytes < 14) { // SHE1 + u16 version + u64 request id
        refuse(1, "malformed", "chunk envelope is shorter than its header");
        return;
    }
    if (m_decodeBacklogFrames >= kMaxDecodeBacklogFrames ||
        bytes > kMaxDecodeBacklogBytes - m_decodeBacklogBytes.load()) {
        refuse(2, "client_overloaded", "chunk decode backlog is full");
        return;
    }
    ++m_decodeBacklogFrames;
    m_decodeBacklogBytes.fetch_add(bytes);
    // Decoding a deep hour takes milliseconds; keep it off the network thread.
    net::post(*m_decodePool, [this, frame = std::move(frame), epoch = m_connectionEpoch.load()] {
        if (epoch == m_connectionEpoch.load()) decodeBinaryMessage(*frame, epoch);
        std::lock_guard lock(m_chunkOrderMutex);
        --m_decodeBacklogFrames;
        m_decodeBacklogBytes.fetch_sub(frame->size());
        if (epoch == m_connectionEpoch.load()) m_decodeRefusals = 0;
    });
}

void SentinelStreamClient::decodeBinaryMessage(const std::vector<uint8_t>& frame, quint64 epoch) {
    heatmap::ChunkEnvelope envelope;
    try {
        // Validates every length and count before allocating (ChunkCodec).
        envelope = m_chunkDecoder(frame);
    } catch (const std::exception& e) {
        std::lock_guard lock(m_chunkOrderMutex);
        if (epoch != m_connectionEpoch.load()) return;
        sLog_Warning("Heatmap chunk frame rejected: bytes=" << frame.size() << " error=" << e.what());
        emit heatmapChunkFailed({peekEnvelopeRequestId(frame), {}, QStringLiteral("malformed"),
                                 QString::fromUtf8(e.what())});
        return;
    }
    std::lock_guard lock(m_chunkOrderMutex);
    if (epoch != m_connectionEpoch.load()) return;
    auto& chunk = envelope.chunk;
    if (chunk.kind == heatmap::ChunkKind::Error) {
        sLog_Probe("chunks.error", "req=" << envelope.requestId << " start=" << chunk.key.startMs
                   << " code=" << heatmap::chunkErrorName(chunk.error) << " message=" << chunk.message);
        emit heatmapChunkFailed({envelope.requestId, chunk.key,
                                 QString::fromLatin1(heatmap::chunkErrorName(chunk.error)),
                                 QString::fromUtf8(chunk.message.data(), qsizetype(chunk.message.size()))});
        return;
    }
    if (chunk.kind == heatmap::ChunkKind::LiveColumn) {
        sLog_Probe("chunks.live.receive", "sub=" << envelope.requestId << " symbol=" << chunk.key.symbol
            << " source=" << chunk.key.source << " revision=" << chunk.state.revision << " bytes=" << frame.size());
        emit heatmapLiveReceived(envelope.requestId, std::make_shared<const heatmap::ChunkFrame>(std::move(chunk)));
        return;
    }
    if (!acceptChunkOrder(chunk)) {
        emit heatmapChunkFailed({envelope.requestId, chunk.key, QStringLiteral("superseded"),
                                 QStringLiteral("a newer revision of this chunk was already delivered")});
        return;
    }
    sLog_Probe("chunks.receive", "req=" << envelope.requestId << " source=" << chunk.key.source
               << " start=" << chunk.key.startMs << " kind=" << int(chunk.kind) << " sealed=" << chunk.state.sealed
               << " rev=" << chunk.state.revision << " bytes=" << frame.size());
    emit heatmapChunkReceived(envelope.requestId, std::make_shared<const heatmap::ChunkFrame>(std::move(chunk)));
}

// Replies for one key can finish out of order on the server's workers. Keep the
// newest: sealed beats open, then (revision, committedThroughMs) must not go back.
bool SentinelStreamClient::acceptChunkOrder(const heatmap::ChunkFrame& frame) {
    const ChunkOrder incoming{frame.state.sealed, frame.state.revision, frame.state.committedThroughMs};
    // Caller holds m_chunkOrderMutex through publication.
    const auto key = std::make_tuple(frame.key.symbol, frame.key.source, frame.key.levelMs, frame.key.startMs);
    const auto it = m_chunkOrder.find(key);
    if (it != m_chunkOrder.end()) {
        const auto& prev = it->second;
        if (!incoming.sealed && (prev.sealed ||
            std::tie(incoming.revision, incoming.committedThroughMs) <
                std::tie(prev.revision, prev.committedThroughMs)))
            return false;
        it->second = incoming;
        return true;
    }
    if (m_chunkOrder.size() >= kMaxChunkOrderKeys) m_chunkOrder.clear(); // bounded; ordering restarts
    m_chunkOrder.emplace(key, incoming);
    return true;
}

void SentinelStreamClient::requestFootprintHistory(const std::string& symbol,
                                                   int64_t timeframeMs,
                                                   int64_t endTimeMs,
                                                   int count) {
    if (symbol.empty() || timeframeMs <= 0 || count <= 0) {
        sLog_Warning("Footprint history request not sent: invalid args symbol=" << symbol
                     << " tfMs=" << timeframeMs << " count=" << count);
        return;
    }
    sLog_Data("Footprint history request: symbol=" << symbol << " tfMs=" << timeframeMs
              << " end=" << endTimeMs << " count=" << count);
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
                                             int sessionType,
                                             int64_t endTimeMs,
                                             int count,
                                             const std::string& requestId) {
    if (symbol.empty() || timeframeMs <= 0 || count <= 0 || requestId.empty() || requestId.size() > 64) {
        sLog_Warning("TPO history request not sent: invalid args symbol=" << symbol
                     << " tfMs=" << timeframeMs << " count=" << count << " requestId=" << requestId);
        return;
    }
    sLog_Data("TPO history request: symbol=" << symbol << " tfMs=" << timeframeMs
              << " sessionType=" << sessionType << " end=" << endTimeMs << " count=" << count
              << " requestId=" << requestId);
    nlohmann::json msg = {
        {"type", "tpo_history_request"},
        {"symbol", symbol},
        {"timeframe_ms", timeframeMs},
        {"session_type", sessionType},
        {"end_time", endTimeMs},
        {"count", count},
        {"request_id", requestId}
    };
    std::string str = msg.dump();
    net::post(m_strand, [this, requestId, payload = std::move(str)]() mutable {
        m_expectedTpoRequestId = requestId;
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected && m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}

void SentinelStreamClient::cancelTpoHistory(const std::string& symbol, const std::string& requestId) {
    if (symbol.empty() || requestId.empty()) return;
    std::string str = nlohmann::json{{"type", "tpo_history_cancel"}, {"symbol", symbol},
                                     {"request_id", requestId}}.dump();
    net::post(m_strand, [this, requestId, payload = std::move(str)]() mutable {
        if (m_expectedTpoRequestId == requestId) m_expectedTpoRequestId.clear();
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
        sLog_Warning("Candle history request not sent: invalid args symbol=" << symbol
                     << " tfSec=" << timeframeSec);
        return;
    }
    sLog_Data("Candle history request: symbol=" << symbol << " tfSec=" << timeframeSec
              << " endSec=" << endTimeSec << " limit=" << limit);
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
        {"timestamp", command.timestamp},
        {"has_take_profit", command.hasTakeProfit},
        {"take_profit_price", command.takeProfitPrice},
        {"has_stop_loss", command.hasStopLoss},
        {"stop_loss_price", command.stopLossPrice}
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
    if (!m_running) return;
    if (ec) {
        sLog_Error("Connect failed: host=" << m_host << " port=" << m_port
                   << " error=" << ec.message());
        emit errorOccurred(QString::fromStdString(ec.message()));
        return;
    }
    
    m_ws->next_layer().async_handshake(
        ssl::stream_base::client,
        [this](auto ec) { onSslHandshake(ec); });
}

void SentinelStreamClient::onSslHandshake(boost::beast::error_code ec) {
    if (!m_running) return;
    if (ec) {
        sLog_Error("SSL handshake failed: host=" << m_host << " port=" << m_port
                   << " error=" << ec.message());
        emit errorOccurred(QString::fromStdString("SSL: " + ec.message()));
        return;
    }

    m_ws->async_handshake(m_host, "/", [this](auto ec) { onHandshake(ec); });
}

void SentinelStreamClient::onHandshake(boost::beast::error_code ec) {
    if (!m_running) return;
    if (ec) {
        sLog_Error("WebSocket handshake failed: host=" << m_host << " port=" << m_port
                   << " error=" << ec.message());
        emit errorOccurred(QString::fromStdString(ec.message()));
        return;
    }

    sLog_Data("SentinelStreamClient connected: host=" << m_host << " port=" << m_port);
    m_isConnected = true;
    // Drain pre-handshake messages here, before notifying subscribers. Posting
    // another drain after connected() races a subscription's own write kick.
    doWrite();
    emit connected();
    
    doRead();

}

void SentinelStreamClient::doRead() {
    if (!m_running) return;
    m_ws->async_read(m_buffer, [this](auto ec, auto bytes) { onRead(ec, bytes); });
}

void SentinelStreamClient::onRead(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (!m_running) return;
    if (ec) {
        if (ec == boost::beast::websocket::error::closed || ec == net::error::operation_aborted) {
            sLog_Data("SentinelStreamClient disconnected: host=" << m_host << " port=" << m_port
                      << " reason=" << ec.message());
        } else {
            sLog_Error("Read failed: host=" << m_host << " port=" << m_port
                       << " error=" << ec.message());
        }
        m_isConnected = false;
        emit disconnected();
        return;
    }
    
    if (m_ws->got_binary()) {
        auto frame = std::make_shared<std::vector<uint8_t>>(bytes_transferred);
        net::buffer_copy(net::buffer(*frame), m_buffer.data());
        m_buffer.consume(bytes_transferred);
        handleBinaryMessage(std::move(frame));
        doRead();
        return;
    }
    std::string msg = boost::beast::buffers_to_string(m_buffer.data());
    m_buffer.consume(bytes_transferred);
    
    handleMessage(msg);
    
    doRead();
}

void SentinelStreamClient::doWrite() {
    if (!m_isConnected || m_writeInFlight || m_writeQueue.empty()) {
        return;
    }
    m_writeInFlight = true;
    m_ws->async_write(net::buffer(m_writeQueue.front()),
                     net::bind_executor(m_strand, [this](auto ec, auto bytes) { onWrite(ec, bytes); }));
}

void SentinelStreamClient::onWrite(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (!m_writeInFlight) {
        sLog_Warning("Client write completion without an outstanding write: host=" << m_host);
        return;
    }
    m_writeInFlight = false;
    if (ec) {
        m_isConnected = false; // Do not retry an ambiguous partial message.
        if (!m_running) return;
        sLog_Error("Write failed: host=" << m_host << " port=" << m_port
                   << " queued=" << m_writeQueue.size() << " error=" << ec.message());
        emit errorOccurred(QString::fromStdString("Write: " + ec.message()));
        return;
    }
    if (m_writeQueue.empty()) {
        sLog_Warning("Client write completion with empty queue: host=" << m_host);
        return;
    }
    m_writeQueue.pop_front();
    doWrite();
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
            case protocol::MessageType::HeatmapRecordingLive:
                if (validateFamilySchema(msg, "heatmap", protocol::SentinelProtocol::kHeatmapSchemaVersion,
                                         DropReason::HeatmapSchema)) {
                    if (auto page = parseRecordingHistoryChunk(msg); page && page->columns.size() <= 2)
                        emit recordingHeatmapLiveReceived(*page);
                }
                return;
            case protocol::MessageType::HeatmapHistoryChunk:
                handleHeatmapHistoryChunkMessage(msg);
                return;
            case protocol::MessageType::HeatmapAvailability: {
                const auto availability = protocol::chunkwire::parseAvailability(msg);
                if (availability.chunkWireVersion != heatmap::kChunkWireVersion) {
                    sLog_Error("Heatmap chunk wire version mismatch: server=" << availability.chunkWireVersion
                               << " client=" << heatmap::kChunkWireVersion << " (chunks refused)");
                    emit errorOccurred(QStringLiteral("heatmap chunk wire version mismatch"));
                    return;
                }
                emit heatmapAvailabilityReceived(availability);
                return;
            }
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
            case protocol::MessageType::RiskOrderUpdate:
                handleRiskOrderUpdateMessage(msg);
                return;
            case protocol::MessageType::ScreenerUpdate:
                handleScreenerUpdateMessage(msg);
                return;
            case protocol::MessageType::VolumeProfileSlice:
                handleVolumeProfileSliceMessage(msg);
                return;
            case protocol::MessageType::CoinbaseLatency:
                handleCoinbaseLatencyMessage(msg);
                return;
            case protocol::MessageType::AlgoOrderEvent:
                handleAlgoOrderEventMessage(msg);
                return;
            case protocol::MessageType::PnlSnapshot:
                handlePnlSnapshotMessage(msg);
                return;
            case protocol::MessageType::Error:
                if (msg.value("context", "") == "screener_request") {
                    emit screenerRequestError(QString::fromStdString(msg.value("message", "")));
                }
                if (msg.value("context", "") == "subscribe" &&
                    (msg.value("code", "") == "connection_cap" || msg.value("code", "") == "invalid_product" ||
                     msg.value("code", "") == "upstream_unavailable")) {
                    emit subscriptionRefused(QString::fromStdString(msg.value("symbol", "")),
                        msg.value("max_connections", 0), QString::fromStdString(msg.value("message", "")));
                }
                if (msg.value("context", "") == "heatmap_recording_view") {
                    emit recordingViewError(QString::fromStdString(msg.value("symbol", "")),
                        msg.value("band_generation", uint64_t{0}), QString::fromStdString(msg.value("code", "")),
                        QString::fromStdString(msg.value("message", "")), msg.value("retry_ms", 1000));
                }
                if (msg.value("context", "") == "heatmap_history_request" &&
                    msg.contains("request_id") && msg["request_id"].is_string()) {
                    emit recordingHeatmapHistoryError(
                        QString::fromStdString(msg.value("symbol", "")),
                        QString::fromStdString(msg["request_id"].get<std::string>()),
                        msg.value("band_generation", uint64_t{0}),
                        QString::fromStdString(msg.value("message", "")));
                }
                if (msg.value("context", "") == "trade_overlay" &&
                    msg.contains("request_id") && msg["request_id"].is_string() &&
                    !m_expectedTpoRequestId.empty() &&
                    msg["request_id"].get<std::string>() == m_expectedTpoRequestId) {
                    m_expectedTpoRequestId.clear();
                    emit tpoHistoryFailed(QString::fromStdString(msg.value("symbol", "")),
                                          QString::fromStdString(msg["request_id"].get<std::string>()),
                                          QString::fromStdString(msg.value("message", "")));
                }
                if (msg.value("context", "") == "candle_history_request") {
                    emit candleHistoryFailed(QString::fromStdString(msg.value("symbol", "")));
                }
                // Log server-side refusals as well as surfacing correlated failures.
                sLog_Warning("Server error: context=" << msg.value("context", "")
                             << " symbol=" << msg.value("symbol", "")
                             << " message=" << msg.value("message", ""));
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
        if (typeStr == "ack") {
            sLog_Data("Server ack: symbol=" << msg.value("symbol", ""));
            if (msg.contains("symbol") && msg["symbol"].is_string())
                emit subscriptionAcknowledged(QString::fromStdString(msg["symbol"].get<std::string>()));
            return;
        }
        logDroppedMessage(DropReason::UnknownType,
                          QString("Dropping message with unhandled type=%1 bytes=%2")
                              .arg(QString::fromStdString(typeStr))
                              .arg(static_cast<qulonglong>(msgStr.size())));

    } catch (const std::exception& e) {
        sLog_Error("Message parse error: bytes=" << msgStr.size()
                   << " head=" << msgStr.substr(0, 120) << " error=" << e.what());
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
    const double tickSize = msg.value("tick_size", 0.0);
    const uint64_t bookVersion = msg.value("book_version", uint64_t{0});
    const auto generation = bookDeliveryGeneration(symbol);
    const auto status = msg.value("book_status", std::string(tickSize > 0.0 ? "ready" : "unavailable"));
    emit snapshotReceived(QString::fromStdString(symbol), bids, asks, tickSize, generation,
                          QString::fromStdString(status), bookVersion);
}

void SentinelStreamClient::handleL2UpdateMessage(const nlohmann::json& msg) {
    const std::string symbol = msg.value("product_id", "");
    if (symbol.empty() || !msg.contains("deltas")) {
        return;
    }
    const auto updates = protocol::clientparse::parseL2Updates(msg["deltas"]);
    const double tickSize = msg.value("tick_size", 0.0);
    const uint64_t bookVersion = msg.value("book_version", uint64_t{0});
    const auto generation = bookDeliveryGeneration(symbol);
    emit l2UpdateReceived(QString::fromStdString(symbol), updates, tickSize, generation, bookVersion);
}

void SentinelStreamClient::handleTradeMessage(const nlohmann::json& msg) {
    emit tradeReceived(protocol::clientparse::parseTrade(msg));
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

    sLog_Probe("heatmap.recv",
               "symbol=" << symbol
               << " tfMs=" << timeframeMs
               << " start=" << startMs << " end=" << endMs
               << " grid=" << gridWidth << "x" << gridHeight
               << " reset=" << reset
               << " bytes=" << column.size() << " liqBytes=" << liquidityColumn.size());

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

std::optional<SentinelStreamClient::RecordingHistoryPage>
SentinelStreamClient::parseRecordingHistoryChunk(const nlohmann::json& msg) {
    RecordingHistoryPage page;
    page.symbol = QString::fromStdString(msg.value("symbol", ""));
    page.requestId = QString::fromStdString(msg.value("request_id", ""));
    page.status = QString::fromStdString(msg.value("status", ""));
    page.message = QString::fromStdString(msg.value("message", ""));
    page.layer = QString::fromStdString(msg.value("layer", ""));
    page.valueEncoding = QString::fromStdString(msg.value("value_encoding", ""));
    page.timeframeMs = msg.value("timeframe_ms", int64_t{0});
    page.requestEndMs = msg.value("request_end_time", int64_t{0});
    page.scannedStartMs = msg.value("scanned_start", int64_t{0});
    page.scannedEndMs = msg.value("scanned_end", int64_t{0});
    page.nextEndMs = msg.value("next_end", int64_t{0});
    page.oldestAvailableMs = msg.value("oldest_available_ms", int64_t{0});
    page.latestAvailableMs = msg.value("latest_available_ms", int64_t{0});
    page.bandGeneration = msg.value("band_generation", uint64_t{0});
    page.exhausted = msg.value("exhausted", false);
    page.bandLo = msg.value("band_lo", 0.0);
    page.bandTick = msg.value("band_tick", 0.0);
    page.bandRows = msg.value("band_rows", 0);
    page.sizeFloor = msg.value("size_floor", 0.0);
    page.codesPerOctave = msg.value("codes_per_octave", 0.0);
    const auto columns = msg.value("columns", nlohmann::json::array());
    if (page.symbol.isEmpty() || page.bandRows < 1 || page.bandRows > 16384 ||
        msg.value("encoding", "") != "base64" ||
        page.valueEncoding != "absolute_log_size" || !columns.is_array() ||
        columns.size() > protocol::SentinelProtocol::kMaxHeatmapHistoryColumns) {
        sLog_Warning("Dropping malformed recording history header: symbol=" << page.symbol
                     << " rows=" << page.bandRows << " columns=" << columns.size());
        return std::nullopt;
    }
    page.columns.reserve(static_cast<int>(columns.size()));
    for (const auto& item : columns) {
        HeatmapHistoryColumn col;
        col.bucketStartMs = item.value("time_start", int64_t{0});
        col.bucketEndMs = item.value("time_end", int64_t{0});
        col.minPrice = item.value("min_price", page.bandLo);
        col.maxPrice = item.value("max_price", page.bandLo + page.bandRows * page.bandTick);
        col.tickSize = item.value("tick_size", page.bandTick);
        col.liquidityScale = item.value("liquidity_scale", 0.0);
        col.observedMs = item.value("observed_ms", uint64_t{0});
        col.flags = item.value("flags", uint32_t{0});
        for (const auto& [name, dest] : {
                 std::pair{"column", &col.intensity},
                 {"liquidity_column", &col.liquidity}, {"validity", &col.validity}}) {
            if (!decodeBase64WithGuardrails("heatmap_history_chunk", name,
                                            item.value(name, ""),
                                            DropReason::HeatmapHistoryPayloadEstimate,
                                            DropReason::HeatmapHistoryBase64Decode,
                                            DropReason::HeatmapHistoryPayloadDecoded,
                                            *dest)) return std::nullopt;
        }
        if (col.intensity.size() != page.bandRows * 2 ||
            col.liquidity.size() != page.bandRows * 2 ||
            col.validity.size() != (page.bandRows + 7) / 8) {
            sLog_Warning("Dropping malformed recording history column: symbol=" << page.symbol
                         << " rows=" << page.bandRows << " time=" << col.bucketStartMs);
            return std::nullopt;
        }
        page.columns.push_back(std::move(col));
    }
    return page;
}

void SentinelStreamClient::handleHeatmapHistoryChunkMessage(const nlohmann::json& msg) {
    if (!validateFamilySchema(msg,
                              "heatmap",
                              protocol::SentinelProtocol::kHeatmapSchemaVersion,
                              DropReason::HeatmapSchema)) {
        return;
    }
    if (msg.value("source", std::string("legacy")) == "recording") {
        if (auto page = parseRecordingHistoryChunk(msg))
            emit recordingHeatmapHistoryReceived(*page);
        return;
    }
    const std::string symbol = msg.value("symbol", "");
    if (symbol.empty()) {
        return;
    }
    const int64_t timeframeMs = msg.value("timeframe_ms", static_cast<int64_t>(0));
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const int64_t requestEndMs = msg.value("request_end_time", static_cast<int64_t>(0));
    const int64_t oldestAvailableMs = msg.value("oldest_available_ms", static_cast<int64_t>(0));
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

    {
        const int count = out.size();
        const int64_t first = count > 0 ? out.front().bucketStartMs : 0;
        const int64_t last = count > 0 ? out.back().bucketStartMs : 0;
        sLog_Data("Heatmap history recv: symbol=" << symbol << " tfMs=" << timeframeMs
                  << " count=" << count << " first=" << first << " last=" << last
                  << " requestEnd=" << requestEndMs << " oldestAvailable=" << oldestAvailableMs
                  << " grid=" << gridWidth << "x" << gridHeight);
    }

    emit heatmapHistoryReceived(QString::fromStdString(symbol), timeframeMs, gridWidth, gridHeight,
                                requestEndMs, oldestAvailableMs, out);
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

    {
        const int count = out.size();
        const int64_t first = count > 0 ? out.front().timeStartMs : 0;
        const int64_t last = count > 0 ? out.back().timeStartMs : 0;
        sLog_Data("Candle history recv: symbol=" << symbol << " tfSec=" << timeframeSec
                  << " count=" << count << " first=" << first << " last=" << last
                  << " startSec=" << startTimeSec << " endSec=" << endTimeSec);
    }

    emit candleHistoryReceived(QString::fromStdString(symbol), timeframeSec, startTimeSec, endTimeSec, out);
}

void SentinelStreamClient::handleCandleBarMessage(protocol::MessageType type, const nlohmann::json& msg) {
    const auto generation = m_candleDeliveryGeneration.load(std::memory_order_acquire);
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
        emit candleBarClosedReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar, generation);
    } else {
        emit candleBarUpdateReceived(symbolQ, timeframeSec, bucketStartMs, seq, bar, generation);
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
    sLog_Probe("footprint.recv",
               "symbol=" << slice.symbol
               << " t=[" << slice.bucketStartMs << ".." << slice.bucketEndMs << "]"
               << " tfMs=" << slice.timeframeMs
               << " grid=" << slice.gridWidth << "x" << slice.gridHeight
               << " bytes=" << slice.deltaLevelsQ16.size());
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
        sLog_Warning("Dropping footprint_history_chunk: columns is not an array, symbol=" << symbol
                     << " tfMs=" << timeframeMs);
        return;
    }

    int emitted = 0;
    int skipped = 0;
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
            ++skipped;
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
            ++skipped;
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

    sLog_Data("Footprint history recv: symbol=" << symbol << " tfMs=" << timeframeMs
              << " columns=" << columns.size() << " emitted=" << emitted);
    if (skipped > 0) {
        sLog_Warning("Footprint history recv: skipped " << skipped
                     << " columns with invalid metadata or payload size, symbol=" << symbol
                     << " tfMs=" << timeframeMs << " gridHeight=" << gridHeight);
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
    update.limitPrice = msg.value("limit_price", 0.0);
    update.algoId = msg.value("algo_id", "");
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
    update.realizedPnl = msg.value("realized_pnl", 0.0);
    if (!update.symbol.empty()) {
        emit positionUpdated(update);
    }
}

void SentinelStreamClient::handleRiskOrderUpdateMessage(const nlohmann::json& msg) {
    trading::RiskOrderUpdate update;
    update.symbol = msg.value("symbol", "");
    update.hasTakeProfit = msg.value("has_take_profit", false);
    update.takeProfitPrice = msg.value("take_profit_price", 0.0);
    update.hasStopLoss = msg.value("has_stop_loss", false);
    update.stopLossPrice = msg.value("stop_loss_price", 0.0);
    if (!update.symbol.empty()) {
        emit riskOrderUpdated(update);
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
    const int sessionType = msg.value("session_type", 4);
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
        logDroppedMessage(DropReason::TpoSliceMeta,
                          QString("Dropping tpo_slice: invalid metadata symbol=%1 start=%2 end=%3 tf=%4 tick=%5 range=[%6,%7]")
                              .arg(QString::fromStdString(symbol))
                              .arg(startMs)
                              .arg(endMs)
                              .arg(timeframeMs)
                              .arg(tickSize, 0, 'g', 8)
                              .arg(minPrice, 0, 'g', 8)
                              .arg(maxPrice, 0, 'g', 8));
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
        logDroppedMessage(DropReason::TpoPayloadShape,
                          QString("Dropping tpo_slice: letters bytes=%1 expected=%2 symbol=%3")
                              .arg(letters.size())
                              .arg(gridHeight)
                              .arg(QString::fromStdString(symbol)));
        return;
    }

    TpoSlice slice;
    slice.symbol = QString::fromStdString(symbol);
    slice.bucketStartMs = startMs;
    slice.bucketEndMs = endMs;
    slice.timeframeMs = timeframeMs;
    slice.sessionType = sessionType;
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
    const int sessionType = msg.value("session_type", 4);
    const int gridWidth = msg.value("grid_width", 0);
    const int gridHeight = msg.value("grid_height", 0);
    const std::string encoding = msg.value("encoding", "base64");
    const auto columns = msg.value("columns", nlohmann::json::array());

    if (!validateGridHeight("tpo_history_chunk", gridHeight, DropReason::TpoGridHeight)) {
        return;
    }
    if (!columns.is_array()) {
        sLog_Warning("Dropping tpo_history_chunk: columns is not an array, symbol=" << symbol
                     << " tfMs=" << timeframeMs);
        return;
    }

    int emitted = 0;
    int skipped = 0;
    int64_t lastEndMs = 0;
    const std::string requestId = (msg.contains("request_id") && msg["request_id"].is_string())
        ? msg["request_id"].get<std::string>() : std::string{};
    // Only the in-flight page is accepted; stale, cancelled or untagged replies
    // are dropped before any slice reaches the GUI.
    if (requestId.empty() || requestId != m_expectedTpoRequestId) {
        sLog_DataN(5000, "Dropping tpo_history_chunk: request_id=" << requestId
                   << " expected=" << m_expectedTpoRequestId << " symbol=" << symbol);
        return;
    }
    m_expectedTpoRequestId.clear();
    for (const auto& item : columns) {
        const int64_t startMs = item.value("time_start", static_cast<int64_t>(0));
        const int64_t endMs = item.value("time_end", static_cast<int64_t>(0));
        const double minPrice = item.value("min_price", 0.0);
        const double maxPrice = item.value("max_price", 0.0);
        const double tickSize = item.value("tick_size", 0.0);
        const std::string format = item.value("format", "tpo_ascii");
        const std::string encoded = item.value("letters", "");
        if (startMs <= 0 || endMs <= startMs || timeframeMs <= 0 || maxPrice <= minPrice || tickSize <= 0.0) {
            ++skipped;
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
            ++skipped;
            continue;
        }
        TpoSlice slice;
        slice.symbol = QString::fromStdString(symbol);
        slice.bucketStartMs = startMs;
        slice.bucketEndMs = endMs;
        slice.timeframeMs = timeframeMs;
        slice.sessionType = sessionType;
        slice.gridWidth = gridWidth;
        slice.gridHeight = gridHeight;
        slice.minPrice = minPrice;
        slice.maxPrice = maxPrice;
        slice.tickSize = tickSize;
        slice.format = QString::fromStdString(format);
        slice.letters = std::move(letters);
        emit tpoSliceReceived(slice);
        ++emitted;
        lastEndMs = std::max(lastEndMs, endMs);
    }
    sLog_Data("TPO history recv: symbol=" << symbol << " tfMs=" << timeframeMs
              << " sessionType=" << sessionType << " columns=" << columns.size()
              << " emitted=" << emitted << " requestId=" << requestId);
    emit tpoHistoryChunkReceived(QString::fromStdString(symbol), QString::fromStdString(requestId),
                                 timeframeMs, sessionType, lastEndMs, emitted);
    if (skipped > 0) {
        sLog_Warning("TPO history recv: skipped " << skipped
                     << " columns with invalid metadata or letter count, symbol=" << symbol
                     << " tfMs=" << timeframeMs << " gridHeight=" << gridHeight);
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
    const auto session = protocol::clientparse::parseVolumeProfileSessionBounds(msg);
    const int64_t sessionStartMs = session.startMs;
    const int64_t sessionEndMs = session.endMs;
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
    if (!session.valid() || maxPrice <= minPrice || tickSize <= 0.0) {
        logDroppedMessage(DropReason::VolumeProfileSliceMeta,
                          QString("Dropping volume_profile_slice: invalid metadata symbol=%1 session=[%2..%3] tick=%4 range=[%5,%6]")
                              .arg(QString::fromStdString(symbol))
                              .arg(sessionStartMs)
                              .arg(sessionEndMs)
                              .arg(tickSize, 0, 'g', 8)
                              .arg(minPrice, 0, 'g', 8)
                              .arg(maxPrice, 0, 'g', 8));
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

void SentinelStreamClient::handleCoinbaseLatencyMessage(const nlohmann::json& msg) {
    if (!msg.contains("ms") || !msg["ms"].is_number_integer()) {
        return;
    }
    const int ms = msg["ms"].get<int>();
    emit coinbaseLatencyReceived(ms);
}

void SentinelStreamClient::handleAlgoOrderEventMessage(const nlohmann::json& msg) {
    trading::AlgoOrderEvent ev;
    ev.algoId = msg.value("algo_id", "");
    ev.orderId = msg.value("order_id", "");
    ev.symbol = msg.value("symbol", "");
    ev.side = trading::orderSideFromString(msg.value("side", "UNKNOWN"));
    ev.orderType = trading::orderTypeFromString(msg.value("order_type", "LIMIT"));
    ev.price = msg.value("price", 0.0);
    ev.qty = msg.value("qty", 0.0);
    ev.status = trading::orderStatusFromString(msg.value("status", "REJECTED"));
    ev.timestampMs = msg.value("timestamp_ms", static_cast<int64_t>(0));
    if (!ev.algoId.empty()) {
        emit algoOrderEventReceived(ev);
    }
}

void SentinelStreamClient::handlePnlSnapshotMessage(const nlohmann::json& msg) {
    trading::PnlSnapshot snap;
    snap.symbol = msg.value("symbol", "");
    snap.timestampMs = msg.value("timestamp_ms", static_cast<int64_t>(0));
    snap.unrealizedPnl = msg.value("unrealized_pnl", 0.0);
    snap.realizedPnl = msg.value("realized_pnl", 0.0);
    snap.totalPnl = msg.value("total_pnl", 0.0);
    snap.algoId = msg.value("algo_id", "");
    if (!snap.symbol.empty()) {
        emit pnlSnapshotReceived(snap);
    }
}

void SentinelStreamClient::sendAlgoCommand(const std::string& algoId,
                                            const std::string& action,
                                            const std::string& symbol,
                                            const trading::AlgoParams& params) {
    nlohmann::json j;
    j["type"] = "algo_command";
    j["algo_id"] = algoId;
    j["action"] = action;
    j["symbol"] = symbol;
    j["params"] = {
        {"spread_bps", params.spreadBps},
        {"order_qty", params.orderQty},
        {"max_position_qty", params.maxPositionQty},
        {"skew_bps", params.skewBps}
    };
    // Enqueue on io thread
    net::post(m_strand, [this, payload = j.dump()]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_writeQueue.size() == 1) {
            doWrite();
        }
    });
}
