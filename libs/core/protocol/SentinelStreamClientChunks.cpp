// SCK1 replies use the existing bounded FIFO decode pool. The publication lock
// fences both pending ids and connection epochs, including a disconnect during
// payload decode. Consumers must connect queued and retire timed-out requests.
#include "SentinelStreamClient.hpp"
#include "flow/FlowChunkCodec.hpp"
#include "SentinelLogging.hpp"

quint64 SentinelStreamClient::requestChunks(const protocol::chunks::Key& key,
    const std::vector<int64_t>& starts, const std::vector<std::optional<uint64_t>>& hashes) {
    const auto id = ++m_nextChunkRequestId;
    auto payload = protocol::chunks::buildRequest(id, key, starts, hashes).dump();
    {
        std::lock_guard lock(m_chunkOrderMutex);
        if (m_chunkRequests.size() >= kMaxDecodeBacklogFrames)
            throw std::runtime_error("too many pending chunk requests");
        auto& pending = m_chunkRequests[id];
        for (const auto start : starts) { auto k = key; k.startMs = start; pending.push_back(std::move(k)); }
    }
    net::post(m_strand, [this, payload = std::move(payload)]() mutable {
        m_writeQueue.push_back(std::move(payload));
        if (m_isConnected) doWrite();
    });
    return id;
}
void SentinelStreamClient::forgetChunks(quint64 id) {
    std::lock_guard lock(m_chunkOrderMutex); m_chunkRequests.erase(id);
}
void SentinelStreamClient::subscribeChunkAvailability(const std::vector<std::string>& ids) {
    auto payload = protocol::chunks::buildSubscribe(ids).dump();
    net::post(m_strand, [this, payload = std::move(payload)]() mutable {
        if (m_chunkSubscription == payload) return;
        m_chunkSubscription = std::move(payload);
        if (m_isConnected) { resendChunkSubscription(); doWrite(); }
    });
}
void SentinelStreamClient::resendChunkSubscription() {
    if (!m_chunkSubscription.empty()) m_writeQueue.push_back(m_chunkSubscription);
}
void SentinelStreamClient::decodeChunkMessage(const std::vector<uint8_t>& bytes, quint64 epoch) {
    namespace w = protocol::chunks;
    quint64 id = 0;
    if (bytes.size() >= 14 && std::equal(bytes.begin(), bytes.begin() + 4, "SHE1"))
        for (int i = 0; i < 8; ++i) id |= quint64(bytes[6 + i]) << (8 * i);
    try {
        auto envelope = w::decodeEnvelope(bytes);
        auto frame = std::get<w::Frame>(std::move(envelope.frame));
        id = envelope.requestId;
        {
            std::lock_guard lock(m_chunkOrderMutex);
            if (epoch != m_connectionEpoch.load()) return;
            const auto it = m_chunkRequests.find(id);
            if (it == m_chunkRequests.end()) return;
            const bool match = std::any_of(it->second.begin(), it->second.end(),
                [&](const auto& k) { return w::matches(k, frame.key); });
            // A malformed request can be refused with the protocol's sentinel start 0.
            const bool sentinel = frame.kind == w::Kind::Error && frame.error == w::Error::InvalidRequest && frame.key.startMs == 0;
            if (!match && !sentinel) throw std::invalid_argument("chunk reply key differs from request");
        }
        auto reply = std::make_shared<w::Reply>();
        if (frame.kind == w::Kind::Chunk) {
            auto decoded = sentinel::flow::chunks::decode(frame.payload, frame.key,
                uint16_t(heatmap::chunkSpanMs(frame.key.levelMs) / frame.key.levelMs), frame.epoch);
            reply->body = std::make_shared<const sentinel::flow::chunks::FlowChunk>(std::move(decoded));
        }
        std::vector<uint8_t>().swap(frame.payload);
        reply->frame = std::move(frame); reply->wireBytes = bytes.size();
        std::lock_guard lock(m_chunkOrderMutex);
        if (epoch != m_connectionEpoch.load()) return;
        const auto it = m_chunkRequests.find(id);
        if (it == m_chunkRequests.end()) return;
        const auto& f = reply->frame;
        const auto key = std::find(it->second.begin(), it->second.end(), w::requestKey(f.key));
        const bool matched = key != it->second.end();
        if (matched) it->second.erase(key);
        if (!matched || it->second.empty()) m_chunkRequests.erase(it);
        if (f.kind == w::Kind::Error)
            emit chunkFailed({id, w::requestKey(f.key), QString::fromLatin1(heatmap::chunkErrorName(f.error)),
                              QString::fromStdString(f.message)});
        else emit chunkReceived(id, std::move(reply));
    } catch (const std::exception& e) {
        std::lock_guard lock(m_chunkOrderMutex);
        if (epoch != m_connectionEpoch.load()) return;
        ++m_chunkRejected;
        m_chunkRequests.erase(id);
        static sentinel::log_throttle::Site warning;
        uint32_t suppressed = 0;
        if (warning.admit(2000, sentinel::log_throttle::nowMs(), suppressed)) sLog_Warning("Chunk frame rejected: req=" << id << " bytes=" << bytes.size() << " error=" << e.what());
        emit chunkFailed({id, {}, QStringLiteral("malformed"), QString::fromUtf8(e.what())});
    }
}
