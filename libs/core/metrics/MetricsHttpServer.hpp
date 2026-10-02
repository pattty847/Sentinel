#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <cstdint>
#include <unordered_map>

class QTcpServer;
class QTcpSocket;

namespace sentinel::metrics {
class MetricsRegistry;

// Plain HTTP/1.1 on 127.0.0.1 only (no auth: never bind another address).
//   GET /ping    -> 200 "OK"
//   GET /metrics -> 200 MetricsRegistry::render(), Prometheus text 0.0.4
// Anything else is 404 (unknown path), 405 (not GET/HEAD) or 400 (malformed or
// over kMaxRequestBytes). One request per connection, then close. Lives on the
// thread that owns it (the main thread in sentinel-server), so the registry's
// samplers render there.
//
// Limits, so a local client cannot exhaust the recorder's file descriptors or
// memory: at most kMaxConnections sockets are open (extra ones are closed on
// accept), each socket buffers at most kMaxRequestBytes + 1 bytes, and every
// socket is aborted requestTimeoutMs after accept, finished or not.
class MetricsHttpServer : public QObject {
    Q_OBJECT
  public:
    explicit MetricsHttpServer(const MetricsRegistry& registry, QObject* parent = nullptr);
    ~MetricsHttpServer() override;

    // port 0 picks a free port (tests). False on bind failure; see errorString().
    bool listen(uint16_t port);
    uint16_t port() const;
    QString errorString() const;

    static constexpr int kMaxRequestBytes = 16 * 1024;
    static constexpr int kMaxConnections = 8;
    static constexpr int kDefaultRequestTimeoutMs = 5000;
    // Tests shorten it; applies to connections accepted afterwards.
    void setRequestTimeoutMs(int ms) { m_requestTimeoutMs = ms; }
    // Open sockets (accepted and not yet destroyed).
    int activeConnections() const { return static_cast<int>(m_requests.size()); }
    // Connections closed on accept because kMaxConnections were open.
    uint64_t refusedConnections() const { return m_refused; }

  private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void handle(QTcpSocket* socket, const QByteArray& request);
    void respond(QTcpSocket* socket, const QByteArray& status, const QByteArray& contentType,
                 const QByteArray& body, bool headOnly);

    const MetricsRegistry& m_registry;
    QTcpServer* m_server = nullptr;
    int m_requestTimeoutMs = kDefaultRequestTimeoutMs;
    uint64_t m_refused = 0;
    // Request bytes per open socket; an entry exists from accept to destruction.
    struct Request {
        QByteArray bytes;
        bool done = false;
    };
    std::unordered_map<QTcpSocket*, Request> m_requests;
};

} // namespace sentinel::metrics
