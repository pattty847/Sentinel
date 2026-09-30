#pragma once
#include "heatmap/ChunkTransport.hpp"
#include <algorithm>

// Deliberately passive: requests never answer until the test scripts a reply.
// Holds/drops frames without decoding again; reconnect and availability are
// independent events, so tests can prove that neither alone resumes work.
class FakeChunkTransport final : public heatmap::ChunkTransport {
public:
    struct Request {
        quint64 id;
        std::string symbol, source;
        int64_t levelMs;
        std::vector<int64_t> starts;
        std::vector<std::optional<uint64_t>> hashes;
        heatmap::ChunkKey key(size_t i) const { return {symbol, source, levelMs, starts.at(i)}; }
    };
    std::vector<Request> requests;
    struct LiveRequest { quint64 id; std::string symbol; std::vector<std::string> sources; int64_t sinceMs; };
    std::vector<LiveRequest> liveRequests;
    std::vector<std::string> liveUnsubscribes;
    std::vector<std::pair<quint64, heatmap::ChunkFramePtr>> heldLive;
    quint64 subscribeLive(const std::string& symbol, std::vector<std::string> sources, int64_t sinceMs) override {
        const auto id = ++next_;
        liveRequests.push_back({id, symbol, std::move(sources), sinceMs});
        return id;
    }
    void unsubscribeLive(const std::string& symbol) override { liveUnsubscribes.push_back(symbol); }
    void replyLive(quint64 id, heatmap::ChunkFramePtr frame) { emit liveReceived(id, std::move(frame)); }
    void holdLive(quint64 id, heatmap::ChunkFramePtr frame) { heldLive.emplace_back(id, std::move(frame)); }
    void releaseLive(size_t index = 0) {
        auto reply = std::move(heldLive.at(index));
        heldLive.erase(heldLive.begin() + index);
        replyLive(reply.first, std::move(reply.second));
    }
    std::vector<quint64> forgotten;
    void forget(quint64 id) override { forgotten.push_back(id); }
    std::vector<std::pair<quint64, heatmap::ChunkFramePtr>> held;
    quint64 request(const std::string &symbol, const std::string &source, int64_t levelMs,
                    std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> hashes) override {
        const auto id = ++next_;
        requests.push_back({id, symbol, source, levelMs, std::move(starts), std::move(hashes)});
        return id;
    }
    void hold(quint64 id, heatmap::ChunkFramePtr frame) { held.emplace_back(id, std::move(frame)); }
    void releaseHeld(size_t index = 0) {
        auto reply = std::move(held.at(index));
        held.erase(held.begin() + index);
        emit received(reply.first, std::move(reply.second));
    }
    void drop(quint64 id) { std::erase_if(held, [id](const auto &v) { return v.first == id; }); }
    void reply(quint64 id, heatmap::ChunkFramePtr frame) { emit received(id, std::move(frame)); }
    void error(quint64 id, heatmap::OptionalChunkKey key, const QString &code) {
        emit failed(id, std::move(key), code, QStringLiteral("scripted error"));
    }
    void goOnline() { emit connected(); }
    void goOffline() { emit disconnected(); }
    void push(heatmap::ChunkAvailability value) { emit availability(std::move(value)); }
private:
    quint64 next_ = 0;
};
