#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QtWebSockets/QWebSocket>
#include "tradedata.h"

class WebSocketClient : public QObject
{
    Q_OBJECT // This macro is required for any class that uses signals and slots

public:
    explicit WebSocketClient(QObject *parent = nullptr);
    void connectToServer(const QUrl &url);
    void closeConnection();

signals:
    void connected();
    void tradeReady(const Trade &trade);
    void disconnected();

private slots:
    void onConnected();
    void onTextMessageReceived(const QString &message);

private:
    QWebSocket m_webSocket;
};

#endif // WEBSOCKETCLIENT_H 