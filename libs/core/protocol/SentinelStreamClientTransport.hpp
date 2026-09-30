#pragma once
#include "../heatmap/ChunkTransport.hpp"
#include <QPointer>
#include <unordered_map>

class SentinelStreamClient;
namespace protocol {
// Client outlives the adapter. Attach before connecting/subscribing so the
// fetcher observes connected + fresh availability. A replacement client denotes
// a host change, even if its request counter restarts at 1.
class SentinelStreamClientTransport final : public heatmap::ChunkTransport {
    Q_OBJECT
public:
    explicit SentinelStreamClientTransport(SentinelStreamClient &client, QObject *parent = nullptr);
    void setClient(SentinelStreamClient &client);
    quint64 request(const std::string &symbol, const std::string &source, int64_t levelMs,
                    std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) override;
    void forget(quint64 requestId) override;
private:
    QPointer<SentinelStreamClient> client_;
    bool connected_ = false;
    quint64 epoch_ = 0, nextRequest_ = 0;
    struct Request { quint64 id; std::vector<heatmap::ChunkKey> keys; };
    std::unordered_map<quint64, Request> requests_;
    void retire(quint64 wireId, heatmap::OptionalChunkKey key);
    void attach(SentinelStreamClient &client);
};
} // namespace protocol
