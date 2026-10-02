#pragma once
#include <QObject>
#include <QString>
#include <cstdint>

class QTcpServer;
class QTcpSocket;

namespace sentinel::metrics {
class MetricsRegistry;

// Plain HTTP/1.1 on 127.0.0.1 only (no auth: never bind another address).
//   GET /ping    -> 200 "OK"
//   GET /metrics -> 200 MetricsRegistry::render(), Prometheus text 0.0.4
// Anything else is 404 (unknown path), 405 (not GET/HEAD) or 400. One request
// per connection, then close. Lives on the thread that owns it (the main thread
// in sentinel-server), so the registry's samplers render there.
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
    static constexpr int kRequestTimeoutMs = 5000;

  private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void respond(QTcpSocket* socket, const QByteArray& status, const QByteArray& contentType,
                 const QByteArray& body, bool headOnly);

    const MetricsRegistry& m_registry;
    QTcpServer* m_server = nullptr;
};

} // namespace sentinel::metrics
