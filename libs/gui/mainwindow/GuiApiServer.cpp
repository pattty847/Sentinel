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
#include <QSaveFile>
#include <QTcpSocket>
#include <QQuickView>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QWindow>
#include <QWidget>

namespace {
QByteArray statusText(int statusCode) {
    switch (statusCode) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Content";
    case 431: return "Request Header Fields Too Large";
    case 403: return "Forbidden";
    case 408: return "Request Timeout";
    case 429: return "Too Many Requests";
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
                           std::function<AgentApi::StateSnapshot()> stateSnapshot,
                           std::function<AgentApi::ViewportSnapshot()> viewportSnapshot,
                           std::function<std::optional<AgentApi::CandleSnapshot>(const AgentApi::ValidationResult&)> candlesSnapshot,
                           std::function<AgentApi::BookSnapshot(int)> bookSnapshot,
                           std::function<AgentApi::TradesSnapshot(qint64, int)> tradesSnapshot,
                           std::function<void(const heatmap_window::WallQuery&,
                                              std::function<void(heatmap_window::WallsSnapshot)>)> wallsSnapshot,
                           std::function<AgentApi::ControlApply(const QString&, const AgentApi::ControlBody&)> applyControl,
                           std::function<std::pair<quint64, quint64>()> frameAck,
                           std::function<void(quint64)> publishRevision,
                           QObject* parent)
    : QObject(parent),
      m_targetWindow(targetWindow),
      m_heatmapView(heatmapView),
      m_labView(labView),
      m_stateSnapshot(std::move(stateSnapshot)),
      m_viewportSnapshot(std::move(viewportSnapshot)),
      m_candlesSnapshot(std::move(candlesSnapshot)),
      m_bookSnapshot(std::move(bookSnapshot)),
      m_tradesSnapshot(std::move(tradesSnapshot)),
      m_wallsSnapshot(std::move(wallsSnapshot)),
      m_applyControl(std::move(applyControl)), m_frameAck(std::move(frameAck)),
      m_publishRevision(std::move(publishRevision)) {
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
        if (m_openConnections.size() >= 8) {
            respond(socket, 429, AgentApi::jsonBytes(AgentApi::error("too_many_requests", "At most eight requests may be open")), "application/json");
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        socket->setParent(this);
        socket->setReadBufferSize(24580);
        m_openConnections.insert(socket);
        m_requests.insert(socket, {});
        auto* deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        deadline->start(6500);
        connect(deadline, &QTimer::timeout, socket, [this, socket]() {
            if (m_requests.contains(socket) || m_pendingWalls.contains(socket)) {
                respond(socket, 408, AgentApi::jsonBytes(AgentApi::error("request_timeout", "Request timed out")), "application/json");
                m_requests.remove(socket);
                m_pendingWalls.remove(socket);
            } else {
                socket->abort(); // Write deadline for a peer that stops reading.
            }
        });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleRequest(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_requests.remove(socket);
            m_pendingWalls.remove(socket);
            m_openConnections.remove(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (socket->bytesAvailable()) handleRequest(socket);
    }
}

void GuiApiServer::handleRequest(QTcpSocket* socket) {
    if (!socket) {
        return;
    }

    auto it = m_requests.find(socket);
    if (it == m_requests.end()) return;
    const QByteArray bytes = socket->read(24577);
    if (socket->bytesAvailable() > 0) {
        respond(socket, 413, AgentApi::jsonBytes(AgentApi::error("request_too_large", "Request exceeds size limit")), "application/json");
        m_requests.erase(it);
        return;
    }
    const AgentApi::ParseResult parsed = it.value().feed(bytes);
    if (parsed.kind == AgentApi::ParseResult::Kind::Incomplete) return;
    m_requests.erase(it);
    if (parsed.kind == AgentApi::ParseResult::Kind::Error) {
        respond(socket, parsed.status, AgentApi::jsonBytes(AgentApi::error(parsed.code, parsed.message)), "application/json");
        return;
    }
    const QString path = parsed.request.path;
    if (parsed.request.method == "POST") {
        if (!parsed.request.query.isEmpty()) {
            respond(socket, 400, AgentApi::jsonBytes(AgentApi::error("invalid_parameter", "Controls do not accept query parameters")), "application/json");
            return;
        }
        const auto capability = m_stateSnapshot().servedTimeframesMs;
        const auto valid = AgentApi::validateControl(parsed.request, capability);
        if (valid.status != 200) {
            respond(socket, valid.status, AgentApi::jsonBytes(AgentApi::error(valid.code, valid.message)), "application/json");
            return;
        }
        const QString kind = path.mid(QStringLiteral("/api/v1/").size());
        const auto applied = m_applyControl(kind, valid.body);
        if (applied.status != 200) {
            respond(socket, applied.status, AgentApi::jsonBytes(AgentApi::error(applied.code, applied.message)), "application/json");
            return;
        }
        auto op = m_operations.apply(kind, applied.viewportVersion);
        m_publishRevision(op.revision);
        QJsonObject data = applied.data;
        data["operationId"] = op.id;
        data["status"] = op.status;
        respond(socket, 200, AgentApi::jsonBytes(AgentApi::envelope(m_stateSnapshot().meta, data)), "application/json");
        return;
    }
    if (path.startsWith("/api/v1/operations/")) {
        const auto check = AgentApi::validateQuery(parsed.request);
        if (check.status != 200) {
            respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
            return;
        }
        if (auto* deadline = socket->findChild<QTimer*>()) deadline->start(10000);
        waitForOperation(socket, path.mid(QStringLiteral("/api/v1/operations/").size()),
                         QDateTime::currentMSecsSinceEpoch() + check.waitMs, false);
        return;
    }
    if (path == "/api/v1/state") {
        const auto snapshot = m_stateSnapshot();
        const auto check = AgentApi::validateQuery(parsed.request, snapshot.meta.symbol);
        if (check.status != 200) {
            respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
            return;
        }
        respond(socket, 200, AgentApi::jsonBytes(AgentApi::stateJson(snapshot)), "application/json");
        return;
    }
    if (path == "/api/v1/heatmap/state") {
        const auto state = m_stateSnapshot();
        const auto check = AgentApi::validateQuery(parsed.request, state.meta.symbol);
        if (check.status != 200) {
            respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
        } else if (!m_heatmapSnapshot) {
            respond(socket, 503, AgentApi::jsonBytes(AgentApi::error("heatmap_unavailable", "Heatmap service unavailable")), "application/json");
        } else respond(socket, 200, AgentApi::jsonBytes(AgentApi::envelope(state.meta, m_heatmapSnapshot())), "application/json");
        return;
    }
    if (path == "/api/v1/viewport") {
        const auto snapshot = m_viewportSnapshot();
        const auto check = AgentApi::validateQuery(parsed.request, snapshot.meta.symbol);
        if (check.status != 200) {
            respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
            return;
        }
        respond(socket, 200, AgentApi::jsonBytes(AgentApi::viewportJson(snapshot)), "application/json");
        return;
    }
    if (path == "/api/v1/candles" || path == "/api/v1/book" || path == "/api/v1/trades" ||
        path == "/api/v1/heatmap/walls") {
        const auto check = AgentApi::validateQuery(parsed.request, m_stateSnapshot().meta.symbol);
        if (check.status != 200) {
            respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
            return;
        }
        if (path == "/api/v1/candles") {
            const auto snapshot = m_candlesSnapshot(check);
            if (!snapshot) {
                respond(socket, 422, AgentApi::jsonBytes(AgentApi::error("timeframe_unavailable", "The client does not hold candles for this symbol and timeframe")), "application/json");
                return;
            }
            respond(socket, 200, AgentApi::jsonBytes(AgentApi::candlesJson(*snapshot)), "application/json");
        } else if (path == "/api/v1/book") {
            respond(socket, 200, AgentApi::jsonBytes(AgentApi::bookJson(m_bookSnapshot(check.levels))), "application/json");
        } else if (path == "/api/v1/trades") {
            respond(socket, 200, AgentApi::jsonBytes(AgentApi::tradesJson(m_tradesSnapshot(check.windowMs, check.limit))), "application/json");
        } else {
            const auto meta = m_stateSnapshot().meta;
            QPointer<GuiApiServer> server(this);
            QPointer<QTcpSocket> peer(socket);
            m_pendingWalls.insert(socket);
            m_wallsSnapshot(check.walls, [server, peer, meta](heatmap_window::WallsSnapshot data) mutable {
                if (!server || !peer || !server->m_pendingWalls.remove(peer.data()) ||
                    peer->state() != QAbstractSocket::ConnectedState) return;
                if (server->m_stateSnapshot().meta.selectionEpoch != meta.selectionEpoch) {
                    server->respond(peer, 409, AgentApi::jsonBytes(AgentApi::error(
                        "selection_changed", "Symbol or timeframe changed while reading walls")), "application/json");
                } else if (data.status == 409) {
                    server->respond(peer, 409, AgentApi::jsonBytes(AgentApi::error(
                        "recording_required", "Legacy liquidity is absolute, but its intensity side can disagree and rows lack validity")), "application/json");
                } else if (data.status == 422) {
                    server->respond(peer, 422, AgentApi::jsonBytes(AgentApi::error(
                        "scan_limit", "Request exceeds the 16000000-cell scan budget; narrow the time range")), "application/json");
                } else if (data.status != 200) {
                    server->respond(peer, 503, AgentApi::jsonBytes(AgentApi::error(
                        "heatmap_unavailable", "Heatmap processor is unavailable")), "application/json");
                } else {
                    AgentApi::WallsSnapshot result{meta, std::move(data)};
                    result.meta.coverage = result.data.missingColumns == 0 && !result.data.unknownRows &&
                                           result.data.recordedColumns > 0 ? "complete" :
                        result.data.recordedColumns > 0 ? "partial" : "unknown";
                    server->respond(peer, 200, AgentApi::jsonBytes(AgentApi::wallsJson(result)), "application/json");
                }
            });
        }
        return;
    }
    const QUrlQuery legacyQuery(parsed.request.query);
    const auto check = AgentApi::validateQuery(parsed.request);
    if (check.status != 200) {
        respond(socket, check.status, AgentApi::jsonBytes(AgentApi::error(check.code, check.message)), "application/json");
        return;
    }
    QString name = path == "/screenshot" ? legacyQuery.queryItemValue("name") : check.screenshotName;
    if (!name.isEmpty()) name = QFileInfo(name).fileName();
    QString targetName = path == "/screenshot" ? legacyQuery.queryItemValue("target") : check.screenshotTarget;
    if (targetName.isEmpty()) targetName = "main";
    if (path == "/api/v1/screenshot" && !check.afterOperation.isEmpty()) {
        if (auto* deadline = socket->findChild<QTimer*>()) deadline->start(10000);
        waitForOperation(socket, check.afterOperation,
                         QDateTime::currentMSecsSinceEpoch() + check.waitMs, true, name, targetName);
        return;
    }
    if (path == "/api/v1/screenshot" && !name.isEmpty()) {
        const QString fileName = name.endsWith(".png", Qt::CaseInsensitive) ? name : name + ".png";
        if (QFileInfo(QDir(m_screenshotDir).filePath(fileName)).isSymLink()) {
            respond(socket, 403, AgentApi::jsonBytes(AgentApi::error("forbidden_path", "Screenshot destination is a symlink")), "application/json");
            return;
        }
    }

    sendScreenshot(socket, name, targetName);
}

void GuiApiServer::waitForOperation(QTcpSocket* socket, const QString& id, qint64 deadlineMs,
                                    bool screenshot, const QString& name, const QString& target) {
    if (!m_openConnections.contains(socket)) return;
    const auto [revision, frameId] = m_frameAck();
    m_operations.poll(revision, frameId);
    const auto op = m_operations.find(id);
    if (!op) {
        respond(socket, 404, AgentApi::jsonBytes(AgentApi::error("unknown_operation", "Unknown operation ID")), "application/json");
        return;
    }
    if (screenshot && op->status == "rendered") {
        sendScreenshot(socket, name, target, op);
        return;
    }
    if (AgentApi::shouldDefer(*op, QDateTime::currentMSecsSinceEpoch(), deadlineMs)) {
        QTimer::singleShot(25, socket, [this, socket, id, deadlineMs, screenshot, name, target]() {
            waitForOperation(socket, id, deadlineMs, screenshot, name, target);
        });
        return;
    }
    if (screenshot) {
        const bool pending = op->status == "applied";
        respond(socket, pending ? 408 : 409,
                AgentApi::jsonBytes(AgentApi::error(pending ? "render_timeout" : "operation_not_rendered",
                                                    pending ? "Operation did not render before deadline" : "Operation is no longer renderable")),
                "application/json");
        return;
    }
    QJsonObject payload{{"operationId", op->id}, {"status", op->status},
                        {"viewportVersion", QString::number(op->viewportVersion)}};
    if (op->status == "rendered") payload["frameId"] = QString::number(op->frameId);
    respond(socket, 200, AgentApi::jsonBytes(AgentApi::envelope(m_stateSnapshot().meta, payload)), "application/json");
}

void GuiApiServer::sendScreenshot(QTcpSocket* socket, const QString& name, const QString& targetName,
                                  const std::optional<AgentApi::Operation>& operation) {
    if (!name.isEmpty()) {
        const QString fileName = name.endsWith(".png", Qt::CaseInsensitive) ? name : name + ".png";
        if (QFileInfo(QDir(m_screenshotDir).filePath(fileName)).isSymLink()) {
            respond(socket, 403, AgentApi::jsonBytes(AgentApi::error("forbidden_path", "Screenshot destination is a symlink")), "application/json");
            return;
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastScreenshotMs && now - m_lastScreenshotMs < 1000) {
        respond(socket, 429, AgentApi::jsonBytes(AgentApi::error("screenshot_rate_limited", "At most one screenshot per second")), "application/json");
        return;
    }
    m_lastScreenshotMs = now;
    QString error;
    const QString savedPath = captureScreenshot(name, targetName, &error);
    if (savedPath.isEmpty()) {
        respond(socket, 500, AgentApi::jsonBytes(AgentApi::error("screenshot_failed", error.isEmpty() ? "Screenshot failed" : error)), "application/json");
        return;
    }

    QJsonObject payload;
    payload["ok"] = true;
    payload["path"] = savedPath;
    payload["target"] = targetName;
    payload["frameId"] = QString::number(operation ? operation->frameId : m_frameAck().second);
    payload["viewportVersion"] = QString::number(operation ? operation->viewportVersion : m_viewportSnapshot().viewportVersion.value_or(0));
    payload["selectionEpoch"] = QString::number(m_stateSnapshot().meta.selectionEpoch);
    respond(socket, 200, jsonBody(payload), "application/json");
}

void GuiApiServer::respond(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType) {
    if (!socket) {
        return;
    }

    const bool oversized = body.size() > 1024 * 1024;
    const int responseStatus = oversized ? 500 : statusCode;
    const QByteArray payload = oversized
        ? AgentApi::jsonBytes(AgentApi::error("response_too_large", "Response exceeds 1 MiB")) : body;

    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(responseStatus));
    response.append(' ');
    response.append(statusText(responseStatus));
    response.append("\r\n");
    response.append("Content-Type: ");
    response.append(contentType);
    response.append("\r\n");
    response.append("Content-Length: ");
    response.append(QByteArray::number(payload.size()));
    response.append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(payload);

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

    QSaveFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
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
