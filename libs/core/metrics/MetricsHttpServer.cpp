#include "MetricsHttpServer.hpp"
#include "MetricsRegistry.hpp"
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace sentinel::metrics {

namespace {
constexpr const char* kBufferProperty = "sentinelHttpRequest";
constexpr const char* kDoneProperty = "sentinelHttpDone";
}

MetricsHttpServer::MetricsHttpServer(const MetricsRegistry& registry, QObject* parent)
    : QObject(parent), m_registry(registry), m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, &MetricsHttpServer::onNewConnection);
}

MetricsHttpServer::~MetricsHttpServer() = default;

bool MetricsHttpServer::listen(uint16_t port) { return m_server->listen(QHostAddress::LocalHost, port); }

uint16_t MetricsHttpServer::port() const { return m_server->serverPort(); }

QString MetricsHttpServer::errorString() const { return m_server->errorString(); }

void MetricsHttpServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        // A client that never finishes its request does not hold a socket forever.
        QTimer::singleShot(kRequestTimeoutMs, socket, [socket] { socket->abort(); });
    }
}

void MetricsHttpServer::onReadyRead(QTcpSocket* socket) {
    if (socket->property(kDoneProperty).toBool()) {
        socket->readAll();
        return;
    }
    QByteArray request = socket->property(kBufferProperty).toByteArray() + socket->readAll();
    // Wait for the end of the headers so closing never resets a half-read request.
    const bool complete = request.contains("\r\n\r\n") || request.contains("\n\n");
    if (!complete && request.size() < kMaxRequestBytes) {
        socket->setProperty(kBufferProperty, request);
        return;
    }
    socket->setProperty(kDoneProperty, true);
    socket->setProperty(kBufferProperty, QVariant());
    if (!complete) {
        respond(socket, "400 Bad Request", "text/plain", "Bad Request", false);
        return;
    }
    const QList<QByteArray> parts = request.left(request.indexOf('\n')).trimmed().split(' ');
    if (parts.size() < 2) {
        respond(socket, "400 Bad Request", "text/plain", "Bad Request", false);
        return;
    }
    const QByteArray& method = parts[0];
    QByteArray path = parts[1];
    if (const qsizetype q = path.indexOf('?'); q >= 0) path.truncate(q);
    const bool head = method == "HEAD";
    if (method != "GET" && !head) {
        respond(socket, "405 Method Not Allowed", "text/plain", "Method Not Allowed", false);
        return;
    }
    if (path == "/ping") {
        respond(socket, "200 OK", "text/plain", "OK", head);
    } else if (path == "/metrics") {
        const std::string body = m_registry.render();
        respond(socket, "200 OK", QByteArray(MetricsRegistry::kContentType.data(),
                                             static_cast<qsizetype>(MetricsRegistry::kContentType.size())),
                QByteArray::fromStdString(body), head);
    } else {
        respond(socket, "404 Not Found", "text/plain", "Not Found", head);
    }
}

void MetricsHttpServer::respond(QTcpSocket* socket, const QByteArray& status, const QByteArray& contentType,
                                const QByteArray& body, bool headOnly) {
    QByteArray response;
    response.reserve(body.size() + 128);
    response += "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    if (!headOnly) response += body;
    socket->write(response);
    socket->disconnectFromHost(); // closes after the write drains
}

} // namespace sentinel::metrics
