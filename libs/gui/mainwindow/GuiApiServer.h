#pragma once

#include <QObject>
#include <QTcpServer>
#include <QHash>
#include <QSet>
#include <functional>
#include "AgentApiCodec.hpp"

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
                          QObject* parent = nullptr);

    bool start(quint16 port, const QString& screenshotDir);
    void stop();
    QString errorString() const;

private slots:
    void handleNewConnection();

private:
    void handleRequest(QTcpSocket* socket);
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
    QHash<QTcpSocket*, AgentApi::RequestParser> m_requests;
    QSet<QTcpSocket*> m_openConnections;
};
