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
#include <QElapsedTimer>
#include <QRectF>
#include <mutex>
#include <memory>
#include <vector>
#include "../../core/TradeData.h"
#include "../../core/LiquidityTimeSeriesEngine.h"
#include "GridTypes.hpp"

class GridViewState;
class DataCache;  // 🚀 Forward declaration

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();

public slots:
    // Data ingestion (slots for cross-thread invocation)
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(std::shared_ptr<const OrderBook> orderBook);
    void onLiveOrderBookUpdated(const QString& productId);  // Dense LiveOrderBook signal handler
    
    // 🎯 THREADING FIX: Move updateVisibleCells to slots for cross-thread calls
    void updateVisibleCells();

public:
    
    // Configuration
    void setGridViewState(GridViewState* viewState) { m_viewState = viewState; }
    void setDataCache(DataCache* cache) { m_dataCache = cache; }  // 🚀 PHASE 3: Dependency injection
    
    // Data access
    bool hasValidOrderBook() const { return m_hasValidOrderBook; }
    const OrderBook& getLatestOrderBook() const;
    
    // Control
    void clearData();
    void startProcessing();
    void stopProcessing();
    
    // 🚀 PHASE 3: Business logic methods moved from UGR
    void createCellsFromLiquiditySlice(const struct LiquidityTimeSlice& slice);
    void createLiquidityCell(const struct LiquidityTimeSlice& slice, double price, double liquidity, bool isBid);
    QRectF timeSliceToScreenRect(const struct LiquidityTimeSlice& slice, double price) const;
    
    // 🚀 PHASE 3: LTSE delegation methods (moved from UGR)
    void setPriceResolution(double resolution);
    double getPriceResolution() const;
    void addTimeframe(int timeframe_ms);
    int64_t suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const;
    std::vector<struct LiquidityTimeSlice> getVisibleSlices(qint64 timeStart, qint64 timeEnd, double minPrice, double maxPrice) const;
    int getDisplayMode() const;
    
    // 🚀 PHASE 3: Manual timeframe management (preserve existing logic)
    void setTimeframe(int timeframe_ms);
    bool isManualTimeframeSet() const;
    
    // 🚀 PHASE 3: Data access for UGR slim adapter
    const std::vector<struct CellInstance>& getVisibleCells() const { return m_visibleCells; }

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
    DataCache* m_dataCache = nullptr;  // 🚀 PHASE 3: Access to dense LiveOrderBook
    
    // Data state
    std::shared_ptr<const OrderBook> m_latestOrderBook;
    bool m_hasValidOrderBook = false;
    
    // Processing
    QTimer* m_snapshotTimer = nullptr;
    mutable std::mutex m_dataMutex;
    
    // 🚀 PHASE 3: Manual timeframe management (moved from UGR)
    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;
    
    // 🚀 PHASE 3: Visible cells storage (moved from UGR)
    std::vector<struct CellInstance> m_visibleCells;
    
    // 🎯 PRICE LOD: Dynamic price resolution
    double m_priceResolution = 1.0;
};