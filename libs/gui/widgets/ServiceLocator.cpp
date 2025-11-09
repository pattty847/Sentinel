#include "ServiceLocator.hpp"
#include "../../core/marketdata/MarketDataCore.hpp"
#include "../../core/marketdata/cache/DataCache.hpp"
#include <QPointer>

QPointer<MarketDataCore> ServiceLocator::s_marketDataCore;
DataCache* ServiceLocator::s_dataCache = nullptr;

void ServiceLocator::registerMarketDataCore(MarketDataCore* core) {
    s_marketDataCore = core;
}

void ServiceLocator::registerDataCache(DataCache* cache) {
    s_dataCache = cache;
}

MarketDataCore* ServiceLocator::marketDataCore() {
    return s_marketDataCore.data();
}

DataCache* ServiceLocator::dataCache() {
    return s_dataCache;
}

