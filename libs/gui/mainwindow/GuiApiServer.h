#pragma once

#include <QObject>
#include <QTcpServer>
#include <QHash>
#include <QSet>
#include <functional>
#include "AgentApiCodec.hpp"
#include "AgentApiOperations.hpp"

class QTcpSocket;
class QWidget;
class QQuickView;

class GuiApiServer : public QObject {
    Q_OBJECT

public:
    explicit GuiApiServer(QWidget* targetWindow,
                          QQuickView* heatmapView,
                          QQuickView* labView,
                          std::function<AgentApi::StateSnapshot()> stateSnapshot,
                          std::function<AgentApi::ViewportSnapshot()> viewportSnapshot,
                          std::function<std::optional<AgentApi::CandleSnapshot>(const AgentApi::ValidationResult&)> candlesSnapshot,
                          std::function<AgentApi::BookSnapshot(int)> bookSnapshot,
                          std::function<AgentApi::TradesSnapshot(qint64, int)> tradesSnapshot,
                          std::function<AgentApi::ControlApply(const QString&, const AgentApi::ControlBody&)> applyControl,
                          std::function<std::pair<quint64, quint64>()> frameAck,
                          std::function<void(quint64)> publishRevision,
                          QObject* parent = nullptr);

    bool start(quint16 port, const QString& screenshotDir);
    void stop();
    QString errorString() const;

private slots:
    void handleNewConnection();

private:
    void handleRequest(QTcpSocket* socket);
    void waitForOperation(QTcpSocket* socket, const QString& id, qint64 deadlineMs,
                          bool screenshot, const QString& name = {}, const QString& target = "main");
    void sendScreenshot(QTcpSocket* socket, const QString& name, const QString& target,
                        const std::optional<AgentApi::Operation>& operation = std::nullopt);
    void respond(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType);
    QString captureScreenshot(const QString& baseName, const QString& target, QString* error) const;
    QImage grabTargetImage(const QString& target, QString* error) const;

    QTcpServer m_server;
    QWidget* m_targetWindow = nullptr;
    QQuickView* m_heatmapView = nullptr;
    QQuickView* m_labView = nullptr;
    QString m_screenshotDir;
    std::function<AgentApi::StateSnapshot()> m_stateSnapshot;
    std::function<AgentApi::ViewportSnapshot()> m_viewportSnapshot;
    std::function<std::optional<AgentApi::CandleSnapshot>(const AgentApi::ValidationResult&)> m_candlesSnapshot;
    std::function<AgentApi::BookSnapshot(int)> m_bookSnapshot;
    std::function<AgentApi::TradesSnapshot(qint64, int)> m_tradesSnapshot;
    std::function<AgentApi::ControlApply(const QString&, const AgentApi::ControlBody&)> m_applyControl;
    std::function<std::pair<quint64, quint64>()> m_frameAck;
    std::function<void(quint64)> m_publishRevision;
    AgentApi::Operations m_operations;
    qint64 m_lastScreenshotMs = 0;
    QHash<QTcpSocket*, AgentApi::RequestParser> m_requests;
    QSet<QTcpSocket*> m_openConnections;
};
