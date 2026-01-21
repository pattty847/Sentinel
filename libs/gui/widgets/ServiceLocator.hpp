#pragma once

#include <QPointer>

// Forward declarations
class MarketDataCoreQt;
class IGridDataSource;

/**
 * Lightweight service locator for shared services.
 * Uses raw pointers; ownership is managed elsewhere (MainWindowGPU).
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
     * Register the DataSource service.
     */
    static void registerDataSource(IGridDataSource* source);
    
    /**
     * Get the registered MarketDataCoreQt instance.
     * Returns nullptr if not registered or destroyed.
     */
    static MarketDataCoreQt* marketDataCore();
    
    /**
     * Get the registered DataSource instance.
     * Returns nullptr if not registered.
     */
    static IGridDataSource* dataSource();

private:
    static QPointer<MarketDataCoreQt> s_marketDataCore;  // QPointer for QObject
    static IGridDataSource* s_dataSource;  // Raw pointer for non-QObject
};

