#pragma once

#include <QObject>
#include <QTcpServer>

class QTcpSocket;
class QWidget;
class QQuickView;

class GuiApiServer : public QObject {
    Q_OBJECT

public:
    explicit GuiApiServer(QWidget* targetWindow,
                          QQuickView* heatmapView,
                          QQuickView* labView,
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
};
