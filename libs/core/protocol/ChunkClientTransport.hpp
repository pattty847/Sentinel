#pragma once
// Data-thread adapter. Wire ids are remapped so replacing a client whose counter
// starts over cannot accept old queued replies. Attach before connecting client.
#include "ChunkReply.hpp"
#include <QObject>
#include <QPointer>
class SentinelStreamClient;
namespace protocol::chunks {
class Transport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual quint64 request(const Key&, std::vector<int64_t>, std::vector<std::optional<uint64_t>>) = 0;
    virtual void subscribe(const std::vector<std::string>&) = 0;
    virtual void forget(quint64) = 0;
signals:
    void received(quint64 id, protocol::chunks::ReplyPtr reply);
    void failed(protocol::chunks::Failure failure);
    void availability(protocol::chunks::Availability snapshot);
    void connected();
    void disconnected();
    void hostChanged();
};
class ClientTransport final : public Transport {
    Q_OBJECT
public:
    explicit ClientTransport(SentinelStreamClient& client, QObject* parent = nullptr);
    ~ClientTransport() override;
    void setClient(SentinelStreamClient& client);
    quint64 request(const Key&, std::vector<int64_t>, std::vector<std::optional<uint64_t>>) override;
    void subscribe(const std::vector<std::string>&) override;
    void forget(quint64) override;
private:
    QPointer<SentinelStreamClient> client_;
    struct Pending { quint64 local; std::vector<Key> keys; };
    std::map<quint64, Pending> pending_;
    std::vector<std::string> interests_;
    quint64 generation_ = 0, next_ = 0;
    bool online_ = false;
    void attach(SentinelStreamClient&);
    void retire(quint64, std::optional<Key>);
    void clear();
};
}
