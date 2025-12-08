#pragma once
#include "IGridDataSource.hpp"
#include "../../core/marketdata/cache/DataCache.hpp"
#include "../../core/marketdata/MarketDataCore.hpp"

/**
 * @brief Local implementation of IGridDataSource.
 * 
 * Wraps direct access to MarketDataCore and DataCache for the monolith application mode.
 * Forwards signals from MarketDataCore and queries from DataCache.
 */
class LocalGridDataSource : public IGridDataSource {
    Q_OBJECT
public:
    LocalGridDataSource(MarketDataCore* core, DataCache* cache, QObject* parent = nullptr);

    void subscribe(const QString& symbol) override;
    void unsubscribe(const QString& symbol) override;
    
    const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const override;
    std::vector<Trade> getRecentTrades(const std::string& productId) const override;

private:
    MarketDataCore* m_core;
    DataCache* m_cache;
};

