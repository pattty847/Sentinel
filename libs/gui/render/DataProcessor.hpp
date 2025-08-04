#pragma once
#include <QObject>
#include <QTimer>
#include <mutex>
#include "../../core/TradeData.h"   
#include "../../gui/LiquidityTimeSeriesEngine.h"

class GridViewState;

/**
 * DataProcessor - Handles all data processing and management
 * 
 * This component takes over the complex data processing logic from UnifiedGridRenderer,
 * making the renderer a pure UI adapter. It manages:
 * - Trade and order book data ingestion
 * - Liquidity time series processing  
 * - Data caching and cleanup
 * - Viewport initialization
 */
class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();
    
    // Data ingestion
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(const OrderBook& orderBook);
    
    // Configuration
    void setGridViewState(GridViewState* viewState) { m_viewState = viewState; }
    void setLiquidityEngine(LiquidityTimeSeriesEngine* engine) { m_liquidityEngine = engine; }
    
    // Data access
    bool hasValidOrderBook() const { return m_hasValidOrderBook; }
    const OrderBook& getLatestOrderBook() const { return m_latestOrderBook; }
    
    // Control
    void clearData();
    void startProcessing();
    void stopProcessing();

signals:
    void dataUpdated();
    void viewportInitialized();

private slots:
    void captureOrderBookSnapshot();

private:
    void initializeViewportFromTrade(const Trade& trade);
    void initializeViewportFromOrderBook(const OrderBook& orderBook);
    
    // Components
    GridViewState* m_viewState = nullptr;
    LiquidityTimeSeriesEngine* m_liquidityEngine = nullptr;
    
    // Data state
    OrderBook m_latestOrderBook;
    bool m_hasValidOrderBook = false;
    
    // Processing
    QTimer* m_snapshotTimer = nullptr;
    mutable std::mutex m_dataMutex;
};