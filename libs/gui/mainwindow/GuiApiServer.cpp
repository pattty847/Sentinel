#include "GuiApiServer.h"
#include "SentinelLogging.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTcpSocket>
#include <QQuickView>
#include <QUrl>
#include <QUrlQuery>
#include <QWindow>
#include <QWidget>

namespace {
QByteArray statusText(int statusCode) {
    switch (statusCode) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

QByteArray jsonBody(const QJsonObject& obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}
} // namespace

GuiApiServer::GuiApiServer(QWidget* targetWindow,
                           QQuickView* heatmapView,
                           QQuickView* labView,
                           QObject* parent)
    : QObject(parent),
      m_targetWindow(targetWindow),
      m_heatmapView(heatmapView),
      m_labView(labView) {
}

bool GuiApiServer::start(quint16 port, const QString& screenshotDir) {
    if (m_server.isListening()) {
        return true;
    }

    m_screenshotDir = screenshotDir;
    connect(&m_server, &QTcpServer::newConnection, this, &GuiApiServer::handleNewConnection);
    const bool ok = m_server.listen(QHostAddress::LocalHost, port);
    if (ok) {
        sLog_App("GUI API listening on 127.0.0.1:" << m_server.serverPort());
    }
    return ok;
}

void GuiApiServer::stop() {
    if (!m_server.isListening()) {
        return;
    }
    m_server.close();
}

QString GuiApiServer::errorString() const {
    return m_server.errorString();
}

void GuiApiServer::handleNewConnection() {
    while (m_server.hasPendingConnections()) {
        QTcpSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            continue;
        }
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleRequest(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void GuiApiServer::handleRequest(QTcpSocket* socket) {
    if (!socket) {
        return;
    }

    QByteArray buffer = socket->property("buffer").toByteArray();
    buffer.append(socket->readAll());
    if (!buffer.contains("\r\n\r\n")) {
        socket->setProperty("buffer", buffer);
        return;
    }

    socket->setProperty("buffer", QByteArray());
    const QList<QByteArray> lines = buffer.split('\n');
    if (lines.isEmpty()) {
        respond(socket, 400, "{}", "application/json");
        return;
    }

    const QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        respond(socket, 400, "{}", "application/json");
        return;
    }

    const QByteArray method = parts.at(0);
    const QByteArray targetPath = parts.at(1);
    if (method != "GET") {
        respond(socket, 405, "{}", "application/json");
        return;
    }

    const QUrl url(QString::fromUtf8(targetPath));
    const QString path = url.path();
    if (path != "/screenshot") {
        respond(socket, 404, "{}", "application/json");
        return;
    }

    const QUrlQuery query(url);
    QString name = query.queryItemValue("name");
    if (!name.isEmpty()) {
        name = QFileInfo(name).fileName();
    }

    QString targetName = query.queryItemValue("target");
    if (targetName.isEmpty()) {
        targetName = "main";
    }

    QString error;
    const QString savedPath = captureScreenshot(name, targetName, &error);
    if (savedPath.isEmpty()) {
        QJsonObject payload;
        payload["ok"] = false;
        payload["error"] = error.isEmpty() ? "screenshot_failed" : error;
        payload["target"] = targetName;
        respond(socket, 500, jsonBody(payload), "application/json");
        return;
    }

    QJsonObject payload;
    payload["ok"] = true;
    payload["path"] = savedPath;
    payload["target"] = targetName;
    respond(socket, 200, jsonBody(payload), "application/json");
}

void GuiApiServer::respond(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType) {
    if (!socket) {
        return;
    }

    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(statusCode));
    response.append(' ');
    response.append(statusText(statusCode));
    response.append("\r\n");
    response.append("Content-Type: ");
    response.append(contentType);
    response.append("\r\n");
    response.append("Content-Length: ");
    response.append(QByteArray::number(body.size()));
    response.append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(body);

    socket->write(response);
    disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
    socket->disconnectFromHost();
}

QString GuiApiServer::captureScreenshot(const QString& baseName, const QString& target, QString* error) const {
    QDir dir(m_screenshotDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error) {
            *error = "mkdir_failed";
        }
        return {};
    }

    QString fileName = baseName;
    if (fileName.isEmpty()) {
        fileName = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz") + "_" + target;
    }
    if (!fileName.endsWith(".png", Qt::CaseInsensitive)) {
        fileName += ".png";
    }

    const QString fullPath = dir.filePath(fileName);
    const QImage image = grabTargetImage(target, error);
    if (image.isNull()) {
        return {};
    }

    if (!image.save(fullPath, "PNG")) {
        if (error) {
            *error = "save_failed";
        }
        return {};
    }

    sLog_App("Saved screenshot to " << fullPath);
    return fullPath;
}

QImage GuiApiServer::grabTargetImage(const QString& target, QString* error) const {
    if (target == "heatmap") {
        if (!m_heatmapView || !m_heatmapView->isVisible()) {
            if (error) {
                *error = "heatmap_not_visible";
            }
            return {};
        }
        return m_heatmapView->grabWindow();
    }

    if (target == "lab") {
        if (!m_labView || !m_labView->isVisible()) {
            if (error) {
                *error = "lab_not_visible";
            }
            return {};
        }
        return m_labView->grabWindow();
    }

    if (!m_targetWindow) {
        if (error) {
            *error = "no_target_window";
        }
        return {};
    }
    if (!m_targetWindow->isVisible()) {
        if (error) {
            *error = "window_not_visible";
        }
        return {};
    }

    QScreen* screen = nullptr;
    if (m_targetWindow->windowHandle()) {
        screen = m_targetWindow->windowHandle()->screen();
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    const QPixmap pixmap = screen ? screen->grabWindow(m_targetWindow->winId()) : m_targetWindow->grab();
    if (pixmap.isNull()) {
        if (error) {
            *error = "grab_failed";
        }
        return {};
    }

    return pixmap.toImage();
}
