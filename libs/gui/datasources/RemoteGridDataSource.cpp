#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"

RemoteGridDataSource::RemoteGridDataSource(const QString& host, const QString& port, QObject* parent)
    : IGridDataSource(parent)
    , m_client(host.toStdString(), port.toStdString())
{
    connect(&m_client, &SentinelStreamClient::tradeReceived, this, &IGridDataSource::tradeReceived);
    connect(&m_client, &SentinelStreamClient::liveOrderBookUpdated, this, &IGridDataSource::liveOrderBookUpdated);
    connect(&m_client, &SentinelStreamClient::connected, [this]{ emit connectionStatusChanged(true); });
    connect(&m_client, &SentinelStreamClient::disconnected, [this]{ emit connectionStatusChanged(false); });
    connect(&m_client, &SentinelStreamClient::errorOccurred, this, &IGridDataSource::errorOccurred);
}

void RemoteGridDataSource::connectToServer() {
    m_client.connectToServer();
}

void RemoteGridDataSource::subscribe(const QString& symbol) {
    m_client.subscribe(symbol.toStdString());
    
    // Ensure we have a replica ready
    if (m_replicaBooks.find(symbol.toStdString()) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(symbol.toStdString(), LiveOrderBook(symbol.toStdString()));
    }
}

void RemoteGridDataSource::unsubscribe(const QString& symbol) {
    m_client.unsubscribe(symbol.toStdString());
}

const LiveOrderBook& RemoteGridDataSource::getDirectLiveOrderBook(const std::string& productId) const {
    auto it = m_replicaBooks.find(productId);
    if (it != m_replicaBooks.end()) {
        return it->second;
    }
    static LiveOrderBook empty;
    return empty;
}

std::vector<Trade> RemoteGridDataSource::getRecentTrades(const std::string& productId) const {
    // TODO: Implement replica trade cache
    return {};
}

