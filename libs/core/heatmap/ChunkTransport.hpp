#pragma once
#include "../protocol/ChunkWire.hpp"
#include <QObject>
#include <memory>
#include <optional>

// The existing client owns Q_DECLARE_METATYPE for the shared frame type.
// Its declaration must precede moc-generated type registration.
// No client implementation is used here.
Q_MOC_INCLUDE("protocol/SentinelStreamClient.hpp")

namespace heatmap {
using ChunkFramePtr = std::shared_ptr<const ChunkFrame>;
using OptionalChunkKey = std::optional<ChunkKey>;
// Preserve the wire's per-source, per-level cutoffs and grid metadata verbatim.
// In particular, a minute end does not imply an hour has been committed.
struct ChunkAvailability : protocol::chunkwire::Availability {};

// request() and lifetime belong to the heatmap-data thread, alongside the
// fetcher. Implementations return unique nonzero ids and publish asynchronously.
// Consumers must use queued connections, including when testing on one thread.
class ChunkTransport : public QObject {
    Q_OBJECT
public:
    explicit ChunkTransport(QObject *parent = nullptr) : QObject(parent) {}
    virtual quint64 request(const std::string &symbol, const std::string &source, int64_t levelMs,
                            std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) = 0;
    // Retire local request bookkeeping (e.g. after a deadline). This does not
    // cancel server work; any later reply must be ignored by the fetcher.
    virtual void forget(quint64 requestId) { Q_UNUSED(requestId); }
    // Live subscriptions use the same id namespace as requests. The local
    // recording-only transport has no live source and returns 0 (unsupported).
    virtual quint64 subscribeLive(const std::string& symbol, std::vector<std::string> sources, int64_t sinceMs) {
        Q_UNUSED(symbol); Q_UNUSED(sources); Q_UNUSED(sinceMs); return 0;
    }
    virtual void unsubscribeLive(const std::string& symbol) { Q_UNUSED(symbol); }
signals:
    void received(quint64 requestId, heatmap::ChunkFramePtr frame);
    void liveReceived(quint64 subscriptionId, heatmap::ChunkFramePtr frame);
    void failed(quint64 requestId, heatmap::OptionalChunkKey key, QString code, QString message);
    void availability(heatmap::ChunkAvailability value);
    void connected();
    void disconnected();
    void wireVersionMismatch();
    void hostChanged();
};
} // namespace heatmap
Q_DECLARE_METATYPE(heatmap::ChunkKey)
Q_DECLARE_METATYPE(heatmap::OptionalChunkKey)
Q_DECLARE_METATYPE(heatmap::ChunkAvailability)
// ChunkFramePtr is also declared by SentinelStreamClient.hpp under its alias.
