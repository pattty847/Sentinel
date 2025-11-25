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
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>
#include <unordered_set>
#include "../../core/marketdata/model/TradeData.h"
#include "../../core/LiquidityTimeSeriesEngine.h"
#include "GridTypes.hpp"

class GridViewState;
class DataCache;

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();

public slots:
    // Data ingestion (slots for cross-thread invocation)
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(std::shared_ptr<const OrderBook> orderBook);
    void onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);  // Dense LiveOrderBook signal handler
    
    // Move updateVisibleCells to slots for cross-thread calls
    void updateVisibleCells();

public:
    
    // Configuration
    void setGridViewState(GridViewState* viewState) { m_viewState = viewState; }
    void setDataCache(DataCache* cache) { m_dataCache = cache; }
    
    // Trade batching configuration
    void setTradeBatchInterval(std::chrono::milliseconds interval) { m_tradeBatchConfig.batchInterval = interval; }
    void setSignificantPriceThreshold(double threshold) { m_tradeBatchConfig.significantPriceThreshold = threshold; }
    void setMaxBatchSize(size_t maxSize) { m_tradeBatchConfig.maxBatchSize = maxSize; }
    
    std::chrono::milliseconds getTradeBatchInterval() const { return m_tradeBatchConfig.batchInterval; }
    double getSignificantPriceThreshold() const { return m_tradeBatchConfig.significantPriceThreshold; }
    size_t getMaxBatchSize() const { return m_tradeBatchConfig.maxBatchSize; }
    
    // Data access
    bool hasValidOrderBook() const { return m_hasValidOrderBook; }
    const OrderBook& getLatestOrderBook() const;
    
    // Control
    void clearData();
    void startProcessing();
    void stopProcessing();
    
    void createCellsFromLiquiditySlice(const struct LiquidityTimeSlice& slice);
    void createLiquidityCell(const struct LiquidityTimeSlice& slice, double price, float liquidity, bool isBid);
    QRectF timeSliceToScreenRect(const struct LiquidityTimeSlice& slice, double price) const;
    
    void setPriceResolution(double resolution);
    double getPriceResolution() const;
    void addTimeframe(int timeframe_ms);
    int64_t suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const;
    std::vector<struct LiquidityTimeSlice> getVisibleSlices(qint64 timeStart, qint64 timeEnd, double minPrice, double maxPrice) const;
    int getDisplayMode() const;

    // Band-based ingestion configuration
    enum class BandMode { FixedDollar, PercentMid, Ticks };
    void setBandMode(BandMode mode) { m_bandMode = mode; }
    void setBandValue(double value) { m_bandValue = value; }
    BandMode getBandMode() const { return m_bandMode; }
    double getBandValue() const { return m_bandValue; }
    
    void setTimeframe(int timeframe_ms);
    bool isManualTimeframeSet() const;
    
    const std::vector<struct CellInstance>& getVisibleCells() const { return m_visibleCells; }
    // Thread-safe snapshot access for renderer (swap-only on render thread)
    std::shared_ptr<const std::vector<struct CellInstance>> getPublishedCellsSnapshot() const {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        return m_publishedCells;
    }

signals:
    void dataUpdated();
    void viewportInitialized();

private slots:
    void captureOrderBookSnapshot();

private:
    void initializeViewportFromTrade(const Trade& trade);
    void initializeViewportFromOrderBook(const OrderBook& orderBook);
    
    // Trade batching helpers
    void processSignificantTrades();
    bool isSignificantTrade(const Trade& trade, double midPrice) const;
    double calculateMidPrice() const;
    
    // Components
    GridViewState* m_viewState = nullptr;
    LiquidityTimeSeriesEngine* m_liquidityEngine = nullptr;
    DataCache* m_dataCache = nullptr;
    
    // Data state
    std::shared_ptr<const OrderBook> m_latestOrderBook;
    bool m_hasValidOrderBook = false;
    
    // Processing
    QTimer* m_snapshotTimer = nullptr;
    mutable std::mutex m_dataMutex;
    
    // Manual timeframe management
    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;
    
    std::vector<struct CellInstance> m_visibleCells;

    // Renderer handoff buffer: atomically swapped shared_ptr to avoid copies
    mutable std::mutex m_snapshotMutex;
    std::shared_ptr<const std::vector<struct CellInstance>> m_publishedCells;
    
    // Dynamic price resolution
    double m_priceResolution = 1.0;

    // Band-based ingestion settings (centered at mid price)
    BandMode m_bandMode = BandMode::PercentMid; // default to percentage of mid
    double m_bandValue = 0.01;                  // 1% default half-band (i.e., ±1%)

    // Phase 1: feature gate for dense ingestion
    bool m_useDenseIngestion = true;
    
    // Shutdown flag to prevent processing after stopProcessing() is called
    std::atomic<bool> m_shuttingDown{false};

    // Append-only state with viewport version gating
    int64_t m_lastProcessedTime = 0;
    uint64_t m_lastViewportVersion = 0;

    // Track processed slices by time range (slices are reused in memory, so can't use pointers)
    struct SliceTimeRange {
        int64_t startTime;
        int64_t endTime;

        bool operator==(const SliceTimeRange& other) const {
            return startTime == other.startTime && endTime == other.endTime;
        }
    };

    struct SliceTimeRangeHash {
        size_t operator()(const SliceTimeRange& range) const {
            return std::hash<int64_t>()(range.startTime) ^ (std::hash<int64_t>()(range.endTime) << 1);
        }
    };

    std::unordered_set<SliceTimeRange, SliceTimeRangeHash> m_processedTimeRanges;

    // Trade batching configuration and state
    struct TradeBatchConfig {
        std::chrono::milliseconds batchInterval{75};  // Configurable batch interval
        double significantPriceThreshold = 0.001;     // 0.1% price impact threshold
        size_t maxBatchSize = 50;                     // Max trades per batch
    };
    
    struct TradeBatch {
        std::vector<Trade> trades;
        std::chrono::steady_clock::time_point lastFlush = std::chrono::steady_clock::now();
        
        bool shouldFlush(const TradeBatchConfig& config) const {
            auto elapsed = std::chrono::steady_clock::now() - lastFlush;
            return elapsed >= config.batchInterval || trades.size() >= config.maxBatchSize;
        }
        
        void clear() {
            trades.clear();
            lastFlush = std::chrono::steady_clock::now();
        }
    };
    
    TradeBatchConfig m_tradeBatchConfig;
    TradeBatch m_tradeBatch;
};
