#include "MetricsHttpServer.hpp"
#include "MetricsRegistry.hpp"
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <utility>

namespace sentinel::metrics {

MetricsHttpServer::MetricsHttpServer(const MetricsRegistry& registry, QObject* parent)
    : QObject(parent), m_registry(registry), m_server(new QTcpServer(this)) {
    m_server->setMaxPendingConnections(kMaxConnections);
    connect(m_server, &QTcpServer::newConnection, this, &MetricsHttpServer::onNewConnection);
}

MetricsHttpServer::~MetricsHttpServer() {
    // Sockets are children of the QTcpServer child; their destroyed() handlers
    // must not touch m_requests once this object is being torn down.
    for (auto& [socket, request] : m_requests) socket->disconnect(this);
}

bool MetricsHttpServer::listen(uint16_t port) { return m_server->listen(QHostAddress::LocalHost, port); }

uint16_t MetricsHttpServer::port() const { return m_server->serverPort(); }

QString MetricsHttpServer::errorString() const { return m_server->errorString(); }

void MetricsHttpServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (activeConnections() >= kMaxConnections) {
            ++m_refused;
            socket->abort();
            socket->deleteLater();
            continue;
        }
        // Qt stops reading from the kernel once this much is buffered.
        socket->setReadBufferSize(kMaxRequestBytes + 1);
        m_requests.emplace(socket, Request{});
        connect(socket, &QObject::destroyed, this, [this, socket] { m_requests.erase(socket); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        // Absolute deadline from accept: a client that never finishes its
        // request (or never reads the answer) does not hold a socket.
        QTimer::singleShot(m_requestTimeoutMs, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
    }
}

void MetricsHttpServer::onReadyRead(QTcpSocket* socket) {
    const auto it = m_requests.find(socket);
    if (it == m_requests.end()) return;
    Request& request = it->second;
    if (request.done) {
        socket->skip(socket->bytesAvailable()); // discard without allocating
        return;
    }
    // Bounded read: never more than one byte past the limit, which is enough
    // to know the request is too large.
    const qint64 room = kMaxRequestBytes + 1 - request.bytes.size();
    request.bytes += socket->read(room);
    if (request.bytes.size() > kMaxRequestBytes) {
        request.done = true;
        request.bytes = QByteArray();
        respond(socket, "400 Bad Request", "text/plain", "Request Too Large", false);
        return;
    }
    // Wait for the end of the headers so closing never resets a half-read request.
    if (!request.bytes.contains("\r\n\r\n") && !request.bytes.contains("\n\n")) return;
    request.done = true;
    const QByteArray bytes = std::exchange(request.bytes, QByteArray());
    handle(socket, bytes);
}

void MetricsHttpServer::handle(QTcpSocket* socket, const QByteArray& request) {
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
        respond(socket, "200 OK",
                QByteArray(MetricsRegistry::kContentType.data(),
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
