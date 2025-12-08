#include "LocalGridDataSource.hpp"
#include "../../core/marketdata/MarketDataCore.hpp"

LocalGridDataSource::LocalGridDataSource(MarketDataCore* core, DataCache* cache, QObject* parent)
    : IGridDataSource(parent)
    , m_core(core)
    , m_cache(cache)
{
    if (m_core) {
        // Forward signals from MarketDataCore to IGridDataSource signals
        connect(m_core, &MarketDataCore::tradeReceived, this, &IGridDataSource::tradeReceived);
        connect(m_core, &MarketDataCore::liveOrderBookUpdated, this, &IGridDataSource::liveOrderBookUpdated);
        // Note: MarketDataCore doesn't emit orderBookUpdated (shared_ptr), it handles it internally or via other signals.
        // Wait, MarketDataCore does emit liveOrderBookUpdated. 
        // DataProcessor::onOrderBookUpdated takes shared_ptr<OrderBook>. 
        // Let's check MarketDataCore signals again.
        
        connect(m_core, &MarketDataCore::connectionStatusChanged, this, &IGridDataSource::connectionStatusChanged);
        connect(m_core, &MarketDataCore::errorOccurred, this, &IGridDataSource::errorOccurred);
    }
}

void LocalGridDataSource::subscribe(const QString& symbol) {
    if (m_core) {
        m_core->subscribeToSymbols({symbol.toStdString()});
    }
}

void LocalGridDataSource::unsubscribe(const QString& symbol) {
    if (m_core) {
        m_core->unsubscribeFromSymbols({symbol.toStdString()});
    }
}

const LiveOrderBook& LocalGridDataSource::getDirectLiveOrderBook(const std::string& productId) const {
    if (m_cache) {
        return m_cache->getDirectLiveOrderBook(productId);
    }
    static LiveOrderBook emptyBook;
    return emptyBook;
}

std::vector<Trade> LocalGridDataSource::getRecentTrades(const std::string& productId) const {
    if (m_cache) {
        return m_cache->recentTrades(productId);
    }
    return {};
}

