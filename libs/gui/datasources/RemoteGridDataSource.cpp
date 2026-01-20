#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"

RemoteGridDataSource::RemoteGridDataSource(const QString& host, const QString& port, QObject* parent)
    : IGridDataSource(parent)
    , m_client(host.toStdString(), port.toStdString())
{
    connect(&m_client, &SentinelStreamClient::tradeReceived, this, &IGridDataSource::tradeReceived);
    // Connect to new specific signals
    connect(&m_client, &SentinelStreamClient::snapshotReceived, this, &RemoteGridDataSource::onSnapshotReceived);
    connect(&m_client, &SentinelStreamClient::l2UpdateReceived, this, &RemoteGridDataSource::onL2UpdateReceived);
    connect(&m_client, &SentinelStreamClient::heatmapSliceReceived, this, &RemoteGridDataSource::onHeatmapSliceReceived);
    
    connect(&m_client, &SentinelStreamClient::connected, [this]{ emit connectionStatusChanged(true); });
    connect(&m_client, &SentinelStreamClient::disconnected, [this]{ emit connectionStatusChanged(false); });
    connect(&m_client, &SentinelStreamClient::errorOccurred, this, &IGridDataSource::errorOccurred);
}

void RemoteGridDataSource::connectToServer() {
    m_client.connectToServer();
}

void RemoteGridDataSource::subscribe(const QString& symbol) {
    m_client.subscribe(symbol.toStdString());
    
    // Ensure we have a replica ready - but we should wait for snapshot to initialize properly
    // However, create it so we don't return null ref
    std::string s = symbol.toStdString();
    if (m_replicaBooks.find(s) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(s, std::make_unique<LiveOrderBook>(s));
        // Default init to avoid crashes if accessed before snapshot
        if (s == "BTC-USD") {
            m_replicaBooks[s]->initialize(75000.0, 125000.0, 0.01);
        } else {
            m_replicaBooks[s]->initialize(75000.0, 125000.0, 0.01);
        }
    }
}


void RemoteGridDataSource::unsubscribe(const QString& symbol) {
    m_client.unsubscribe(symbol.toStdString());
}

const LiveOrderBook& RemoteGridDataSource::getDirectLiveOrderBook(const std::string& productId) const {
    auto it = m_replicaBooks.find(productId);
    if (it != m_replicaBooks.end() && it->second) {
        return *it->second;
    }
    static LiveOrderBook empty;
    return empty;
}

void RemoteGridDataSource::onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks) {
    std::string symbol = productId.toStdString();
    
    // Create or reset replica
    if (m_replicaBooks.find(symbol) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(symbol, std::make_unique<LiveOrderBook>(symbol));
    }
    
    auto& book = *m_replicaBooks[symbol];
    
    // Re-initialize (clears data)
    // TODO: Protocol should transmit these params!
    if (symbol == "BTC-USD") {
        book.initialize(75000.0, 125000.0, 0.01);
    } else {
        book.initialize(75000.0, 125000.0, 0.01);
    }
    
    std::vector<BookLevelUpdate> updates;
    updates.reserve(bids.size() + asks.size());
    
    for (const auto& level : bids) {
        updates.push_back({true, level.price, level.size});
    }
    for (const auto& level : asks) {
        updates.push_back({false, level.price, level.size});
    }
    
    auto now = std::chrono::system_clock::now();
    // Snapshot doesn't produce deltas that we need to broadcast usually, 
    // unless we want to trigger a full repaint. 
    // But DataProcessor usually repaints on timer or updates.
    // However, we should emit `orderBookUpdated` (shared_ptr) if using legacy path?
    // IGridDataSource interface has `liveOrderBookUpdated` (deltas) and `orderBookUpdated` (full book).
    
    book.applyUpdates(updates, now, nullptr);
    
    sLog_Data(QString("RemoteGridDataSource: Snapshot applied for %1 (%2 bids, %3 asks)")
              .arg(productId).arg(bids.size()).arg(asks.size()));
}

void RemoteGridDataSource::onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates) {
    std::string symbol = productId.toStdString();
    auto it = m_replicaBooks.find(symbol);
    if (it == m_replicaBooks.end()) return;
    
    auto& book = *it->second;
    
    thread_local std::vector<BookDelta> deltas;
    deltas.clear();
    
    auto now = std::chrono::system_clock::now();
    book.applyUpdates(updates, now, &deltas);
    
    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }
}

void RemoteGridDataSource::onHeatmapSliceReceived(const QString& symbol,
                                                  int64_t bucketStartMs,
                                                  int64_t bucketEndMs,
                                                  int64_t timeframeMs,
                                                  double minPrice,
                                                  double maxPrice,
                                                  double tickSize,
                                                  double midPrice,
                                                  double lastTrade,
                                                  const QString& format,
                                                  const QByteArray& column,
                                                  const QByteArray& liquidityColumn,
                                                  double liquidityScale,
                                                  bool reset) {
    emit heatmapSliceReceived(symbol,
                              bucketStartMs,
                              bucketEndMs,
                              timeframeMs,
                              minPrice,
                              maxPrice,
                              tickSize,
                              midPrice,
                              lastTrade,
                              format,
                              column,
                              liquidityColumn,
                              liquidityScale,
                              reset);
}
