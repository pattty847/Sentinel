#pragma once

#include <QPointer>

// Forward declarations
class MarketDataCoreQt;
class DataCache;

/**
 * Lightweight service locator for shared services.
 * Uses raw pointers since DataCache is not a QObject.
 * 
 * Future evolution: Can be extended with signals for service registration/unregistration
 * for MarketDataCoreQt (which is a QObject) to support hot-reload scenarios.
 */
class ServiceLocator {
public:
    /**
     * Register the MarketDataCoreQt service.
     */
    static void registerMarketDataCore(MarketDataCoreQt* core);
    
    /**
     * Register the DataCache service.
     */
    static void registerDataCache(DataCache* cache);
    
    /**
     * Get the registered MarketDataCoreQt instance.
     * Returns nullptr if not registered or destroyed.
     */
    static MarketDataCoreQt* marketDataCore();
    
    /**
     * Get the registered DataCache instance.
     * Returns nullptr if not registered.
     */
    static DataCache* dataCache();

private:
    static QPointer<MarketDataCoreQt> s_marketDataCore;  // QPointer for QObject
    static DataCache* s_dataCache;  // Raw pointer for non-QObject
};

