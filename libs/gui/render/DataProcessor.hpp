/*
Sentinel — DataProcessor
Role: Decouples data processing from rendering by processing market data on a background thread.
Inputs/Outputs: Takes Trade/OrderBook data via slots; emits dataUpdated() when processing is done.
Threading: Lives and operates on a dedicated QThread; receives data from main and signals back.
Performance: Uses a queue and a timer-driven loop to batch-process data efficiently.
Integration: Owned by UnifiedGridRenderer; uses LiquidityTimeSeriesEngine for data aggregation.
Observability: Logs thread status and processing batches via sLog_Render.
Related: DataProcessor.cpp, UnifiedGridRenderer.h, LiquidityTimeSeriesEngine.h, GridViewState.hpp.
Assumptions: Dependencies (GridViewState, LiquidityTimeSeriesEngine) are set before use.
*/
#pragma once
#include <QObject>
#include <QTimer>
#include <mutex>
#include <memory>
#include "../../core/TradeData.h"
#include "../../core/LiquidityTimeSeriesEngine.h"

class GridViewState;

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();
    
    // Data ingestion
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(std::shared_ptr<const OrderBook> orderBook);
    
    // Configuration
    void setGridViewState(GridViewState* viewState) { m_viewState = viewState; }
    void setLiquidityEngine(LiquidityTimeSeriesEngine* engine) { m_liquidityEngine = engine; }
    
    // Data access
    bool hasValidOrderBook() const { return m_hasValidOrderBook; }
    const OrderBook& getLatestOrderBook() const;
    
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
    std::shared_ptr<const OrderBook> m_latestOrderBook;
    bool m_hasValidOrderBook = false;
    
    // Processing
    QTimer* m_snapshotTimer = nullptr;
    mutable std::mutex m_dataMutex;
};