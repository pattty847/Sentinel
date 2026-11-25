#include "ChunkClientTransport.hpp"
#include "SentinelStreamClient.hpp"
#include <QThread>
namespace protocol::chunks {
ClientTransport::ClientTransport(SentinelStreamClient& c, QObject* parent) : Transport(parent) { attach(c); }
ClientTransport::~ClientTransport() { clear(); }
void ClientTransport::clear() {
    if (client_) for (const auto& [id, p] : pending_) client_->forgetChunks(id);
    pending_.clear(); online_ = false;
}
void ClientTransport::setClient(SentinelStreamClient& c) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (client_ == &c) return;
    if (client_) QObject::disconnect(client_, nullptr, this, nullptr);
    clear(); emit disconnected(); emit hostChanged(); attach(c);
    c.subscribeChunkAvailability(interests_);
}
void ClientTransport::attach(SentinelStreamClient& c) {
    client_ = &c; const auto generation = ++generation_;
    connect(&c, &SentinelStreamClient::connected, this, [this, generation] {
        if (generation != generation_) return;
        if (online_ || !pending_.empty()) { clear(); emit disconnected(); }
        online_ = true; emit connected();
    }, Qt::QueuedConnection);
    connect(&c, &SentinelStreamClient::disconnected, this, [this, generation] {
        if (generation != generation_ || (!online_ && pending_.empty())) return;
        clear(); emit disconnected();
    }, Qt::QueuedConnection);
    connect(&c, &SentinelStreamClient::chunkAvailabilityReceived, this, [this, generation](Availability a) {
        if (generation == generation_ && online_) emit availability(std::move(a));
    }, Qt::QueuedConnection);
    connect(&c, &SentinelStreamClient::chunkReceived, this, [this, generation](quint64 id, ReplyPtr reply) {
        if (generation != generation_) return;
        const auto it = pending_.find(id); if (it == pending_.end()) return;
        emit received(it->second.local, reply);
        retire(id, reply ? std::optional(requestKey(reply->frame.key)) : std::nullopt);
    }, Qt::QueuedConnection);
    connect(&c, &SentinelStreamClient::chunkFailed, this, [this, generation](Failure f) {
        if (generation != generation_) return;
        const auto id = f.requestId;
        const auto it = pending_.find(id);
        if (id && it == pending_.end()) return;
        f.requestId = it == pending_.end() ? 0 : it->second.local;
        emit failed(f); retire(id, f.key);
    }, Qt::QueuedConnection);
    // Queue the seed so a service can finish constructing its fetcher first.
    // The atomic read is safe on this thread; normal signals own later changes.
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation != generation_ || online_ || !client_ || !client_->isConnected()) return;
        online_ = true; emit connected();
    }, Qt::QueuedConnection);
}
void ClientTransport::retire(quint64 id, std::optional<Key> key) {
    if (!id) { for (const auto& [wire, p] : pending_) if (client_) client_->forgetChunks(wire); pending_.clear(); return; }
    const auto it = pending_.find(id); if (it == pending_.end()) return;
    const auto found = key ? std::find(it->second.keys.begin(), it->second.keys.end(), *key) : it->second.keys.end();
    const bool matched = found != it->second.keys.end();
    if (matched) it->second.keys.erase(found);
    if (!matched || it->second.keys.empty()) pending_.erase(it);
}
quint64 ClientTransport::request(const Key& key, std::vector<int64_t> starts,
                                std::vector<std::optional<uint64_t>> hashes) {
    Q_ASSERT(thread() == QThread::currentThread());
    const auto id = ++next_;
    try {
        if (!client_) throw std::runtime_error("stream client destroyed");
        const auto wire = client_->requestChunks(key, starts, hashes);
        Pending p{id, {}};
        for (auto start : starts) { auto k = key; k.startMs = start; p.keys.push_back(std::move(k)); }
        pending_[wire] = std::move(p);
    } catch (const std::exception& e) {
        QMetaObject::invokeMethod(this, [this, id, message = QString::fromUtf8(e.what())] {
            emit failed({id, {}, QStringLiteral("client_overloaded"), message});
        }, Qt::QueuedConnection);
    }
    return id;
}
void ClientTransport::subscribe(const std::vector<std::string>& ids) {
    Q_ASSERT(thread() == QThread::currentThread());
    buildSubscribe(ids); interests_ = ids;
    if (client_) client_->subscribeChunkAvailability(ids);
}
void ClientTransport::forget(quint64 id) {
    Q_ASSERT(thread() == QThread::currentThread());
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.local != id) { ++it; continue; }
        if (client_) client_->forgetChunks(it->first);
        it = pending_.erase(it);
    }
}
}
