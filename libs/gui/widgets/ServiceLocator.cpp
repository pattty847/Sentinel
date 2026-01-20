#include "ServiceLocator.hpp"
#include "../marketdata/MarketDataCoreQt.hpp"
#include "../../core/marketdata/cache/DataCache.hpp"
#include <QPointer>

QPointer<MarketDataCoreQt> ServiceLocator::s_marketDataCore;
DataCache* ServiceLocator::s_dataCache = nullptr;

void ServiceLocator::registerMarketDataCore(MarketDataCoreQt* core) {
    s_marketDataCore = core;
}

void ServiceLocator::registerDataCache(DataCache* cache) {
    s_dataCache = cache;
}

MarketDataCoreQt* ServiceLocator::marketDataCore() {
    return s_marketDataCore.data();
}

DataCache* ServiceLocator::dataCache() {
    return s_dataCache;
}

