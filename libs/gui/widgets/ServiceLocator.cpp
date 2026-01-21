#include "ServiceLocator.hpp"
#include "../marketdata/MarketDataCoreQt.hpp"
#include "../datasources/IGridDataSource.hpp"
#include <QPointer>

QPointer<MarketDataCoreQt> ServiceLocator::s_marketDataCore;
IGridDataSource* ServiceLocator::s_dataSource = nullptr;

void ServiceLocator::registerMarketDataCore(MarketDataCoreQt* core) {
    s_marketDataCore = core;
}

void ServiceLocator::registerDataSource(IGridDataSource* source) {
    s_dataSource = source;
}

MarketDataCoreQt* ServiceLocator::marketDataCore() {
    return s_marketDataCore.data();
}

IGridDataSource* ServiceLocator::dataSource() {
    return s_dataSource;
}

