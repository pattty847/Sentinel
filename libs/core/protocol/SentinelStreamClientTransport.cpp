#include "SentinelStreamClient.hpp"
#include "SentinelStreamClientTransport.hpp"
#include "../SentinelLogging.hpp"
#include <algorithm>
#include <QThread>

namespace protocol {
SentinelStreamClientTransport::SentinelStreamClientTransport(SentinelStreamClient &client, QObject *parent)
    : ChunkTransport(parent) { attach(client); }

void SentinelStreamClientTransport::setClient(SentinelStreamClient &client) {
    if (client_ == &client) return;
    if (client_) QObject::disconnect(client_, nullptr, this, nullptr);
    requests_.clear();
    liveRequests_.clear();
    connected_ = false;
    emit disconnected();
    emit hostChanged();
    attach(client);
    sLog_Data("Chunk transport host changed");
}

void SentinelStreamClientTransport::attach(SentinelStreamClient &client) {
    client_ = &client;
    const auto epoch = ++epoch_;
    connect(&client, &SentinelStreamClient::connected, this, [this, epoch] {
        if (epoch != epoch_) return;
        // Explicit disconnectFromServer() drops writes without emitting down.
        // A second up must invalidate that connection's requests and freshness.
        if (connected_ || !requests_.empty() || !liveRequests_.empty()) {
            requests_.clear();
            liveRequests_.clear();
            emit disconnected();
        }
        connected_ = true;
        emit connected();
    }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::disconnected, this, [this, epoch] {
        if (epoch != epoch_) return;
        // Failed retries while already down invalidate nothing. Do not republish
        // the same state (and trigger recurring fetcher/consumer diagnostics).
        if (!connected_ && requests_.empty() && liveRequests_.empty()) return;
        requests_.clear();
        liveRequests_.clear();
        connected_ = false;
        emit disconnected();
    }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::heatmapChunkReceived, this,
        [this, epoch](quint64 id, heatmap::ChunkFramePtr frame) {
            if (epoch != epoch_) return;
            auto it = requests_.find(id);
            if (it == requests_.end()) return;
            const auto key = frame ? heatmap::OptionalChunkKey(frame->key) : std::nullopt;
            emit received(it->second.id, std::move(frame));
            retire(id, key);
        }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::heatmapLiveReceived, this,
        [this, epoch](quint64 id, heatmap::ChunkFramePtr frame) {
            if (epoch != epoch_ || !frame || frame->kind != heatmap::ChunkKind::LiveColumn) return;
            const auto it = liveRequests_.find(frame->key.symbol);
            if (it == liveRequests_.end() || it->second.wireId != id ||
                std::find(it->second.sources.begin(), it->second.sources.end(), frame->key.source) == it->second.sources.end()) return;
            emit liveReceived(it->second.id, std::move(frame));
        }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::heatmapChunkFailed, this,
        [this, epoch](const SentinelStreamClient::HeatmapChunkError &error) {
            if (epoch != epoch_) return;
            const auto it = requests_.find(error.requestId);
            if (it == requests_.end() && error.requestId) {
                for (const auto& [symbol, live] : liveRequests_) if (live.wireId == error.requestId) {
                    emit failed(live.id, error.key.symbol.empty() ? heatmap::OptionalChunkKey{} : error.key,
                                error.code, error.message);
                    return;
                }
                return;
            }
            emit failed(it == requests_.end() ? 0 : it->second.id,
                        error.key.symbol.empty() ? heatmap::OptionalChunkKey{} : error.key,
                        error.code, error.message);
            retire(error.requestId, error.key.symbol.empty() ? heatmap::OptionalChunkKey{} : error.key);
        }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::heatmapAvailabilityReceived, this,
        [this, epoch](const chunkwire::Availability &value) {
            if (epoch == epoch_) emit availability(heatmap::ChunkAvailability{value});
        }, Qt::QueuedConnection);
    connect(&client, &SentinelStreamClient::errorOccurred, this, [this, epoch](const QString &error) {
        // This is the client's public mismatch notification; rejected
        // availability is deliberately not emitted by that client.
        if (epoch == epoch_ && error == QStringLiteral("heatmap chunk wire version mismatch")) {
            requests_.clear();
            liveRequests_.clear();
            emit wireVersionMismatch();
        }
    }, Qt::QueuedConnection);
}

void SentinelStreamClientTransport::retire(quint64 wireId, heatmap::OptionalChunkKey key) {
    if (!wireId) { requests_.clear(); return; }
    const auto it = requests_.find(wireId);
    if (it == requests_.end()) return;
    const auto removed = key ? std::erase(it->second.keys, *key) : 0;
    if (!key || !removed || it->second.keys.empty()) requests_.erase(it);
}

void SentinelStreamClientTransport::forget(quint64 requestId) {
    Q_ASSERT(thread() == QThread::currentThread());
    std::erase_if(requests_, [requestId](const auto &entry) { return entry.second.id == requestId; });
}

quint64 SentinelStreamClientTransport::request(const std::string &symbol, const std::string &source,
    int64_t levelMs, std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) {
    Q_ASSERT(thread() == QThread::currentThread());
    const auto id = ++nextRequest_;
    if (!client_) {
        QMetaObject::invokeMethod(this, [this, id] {
            emit failed(id, {}, QStringLiteral("unavailable"), QStringLiteral("stream client destroyed"));
        }, Qt::QueuedConnection);
        return id;
    }
    std::vector<uint64_t> hashes;
    hashes.reserve(haveHash.size());
    for (const auto hash : haveHash) hashes.push_back(hash.value_or(0));
    // This public API is thread-safe: atomic id allocation + post to asio's
    // strand. No GUI/client QObject state is accessed by the calling thread.
    const auto wireId = client_->requestHeatmapChunks(symbol, source, levelMs, starts, hashes);
    Request pending{id, {}};
    for (const auto start : starts) pending.keys.push_back({symbol, source, levelMs, start});
    requests_[wireId] = std::move(pending);
    return id;
}
quint64 SentinelStreamClientTransport::subscribeLive(const std::string& symbol,
    std::vector<std::string> sources, int64_t sinceMs) {
    Q_ASSERT(thread() == QThread::currentThread());
    const auto id = ++nextRequest_;
    if (!client_) {
        QMetaObject::invokeMethod(this, [this, id] {
            emit failed(id, {}, QStringLiteral("unavailable"), QStringLiteral("stream client destroyed"));
        }, Qt::QueuedConnection);
        return id;
    }
    const auto wireId = client_->subscribeHeatmapLive(symbol, sources, sinceMs);
    liveRequests_[symbol] = {id, wireId, std::move(sources)};
    return id;
}
void SentinelStreamClientTransport::unsubscribeLive(const std::string& symbol) {
    Q_ASSERT(thread() == QThread::currentThread());
    liveRequests_.erase(symbol);
    if (client_) client_->unsubscribeHeatmapLive(symbol);
}

} // namespace protocol
