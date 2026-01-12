#include "DataBootstrapper.h"
#include "../../core/marketdata/MarketDataCore.hpp"
#include "../../core/marketdata/auth/Authenticator.hpp"
#include "../../core/marketdata/cache/DataCache.hpp"
#include "../../core/SentinelLogging.hpp"
#include <QSettings>
#include <QScopeGuard>

#define LOG_SCOPE(msg) sLog_App(msg " started"); auto _scopeGuard = qScopeGuard([=]{ sLog_App(msg " complete"); });

DataComponents DataBootstrapper::initialize() {
    LOG_SCOPE("Initializing data components");
    
    QString keyFile = getKeyFileFromConfig();
    
    DataComponents components;
    components.authenticator = std::make_unique<Authenticator>(keyFile.toStdString());
    components.dataCache = std::make_unique<DataCache>();
    components.marketDataCore = std::make_unique<MarketDataCore>(
        *components.authenticator, 
        *components.dataCache
    );
    components.marketDataCore->start();
    
    return components;
}

QString DataBootstrapper::getKeyFileFromConfig() {
    QSettings config("config.ini", QSettings::IniFormat);
    return config.value("auth/keyFile", "key.json").toString();
}

