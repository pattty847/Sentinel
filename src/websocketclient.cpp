#include "websocketclient.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

WebSocketClient::WebSocketClient(QObject *parent) : QObject(parent)
{
    // Set the parent of the underlying socket to be this object.
    // This ensures that the socket is moved to the worker thread along with the client.
    m_webSocket.setParent(this);

    connect(&m_webSocket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &WebSocketClient::disconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
}

void WebSocketClient::connectToServer(const QUrl &url)
{
    m_webSocket.open(url);
}

void WebSocketClient::closeConnection()
{
    m_webSocket.close();
}

void WebSocketClient::onConnected()
{
    emit connected(); // Pass the signal on
    QString subscription = R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker"]})";
    m_webSocket.sendTextMessage(subscription);
}

void WebSocketClient::onTextMessageReceived(const QString &message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject obj = doc.object();

    if (obj["type"].toString() == "ticker" && obj.contains("price")) {
        // Create a new Trade object on the stack
        Trade trade;

        // Populate the struct with data from the JSON
        trade.timestamp = QDateTime::fromString(obj["time"].toString(), Qt::ISODate);
        trade.price = obj["price"].toString().toDouble();
        trade.size = obj["last_size"].toString().toDouble();
        
        QString sideStr = obj["side"].toString();
        if (sideStr == "buy") {
            trade.side = Side::Buy;
        } else if (sideStr == "sell") {
            trade.side = Side::Sell;
        } else {
            trade.side = Side::Unknown;
        }
        
        // Emit the signal with the structured data object
        emit tradeReady(trade);
    }
} 