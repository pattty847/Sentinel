#pragma once
#include <QObject>
#include <QTimer>
#include <QSettings>
#include <vector>
#include <array>
#include <atomic>
#include <map>
#include "../core/lockfreequeue.h"
#include "../core/tradedata.h"
#include "../core/CoinbaseStreamClient.hpp"
#include "candlelod.h"
#include "fractalzoomcontroller.h"

// Forward declarations for GPU components
namespace GPUTypes {
    struct Point {
        float x, y, r, g, b, a;
    };
}

struct CandleUpdate {
    std::string symbol;
    int64_t timestamp_ms;
    CandleLOD::TimeFrame timeframe;
    OHLC candle;
    bool isNewCandle;
};

using CandleQueue = LockFreeQueue<CandleUpdate, 16384>;

class GPUDataAdapter : public QObject {
    Q_OBJECT

public:
    explicit GPUDataAdapter(QObject* parent = nullptr);
    ~GPUDataAdapter();

    // Initialization
    void setCoinbaseClient(CoinbaseStreamClient* client);
    void setFractalZoomController(FractalZoomController* controller);
    void setSymbol(const std::string& symbol);
    void start();
    void stop();

    // Configuration
    void setReserveSize(size_t size);
    void setFirehoseRate(int rate);
    void setOrderBookDepth(size_t depth);
    void setTimeFrame(CandleLOD::TimeFrame timeframe);

    // 🚀 LOCK-FREE PIPELINE: Push trades to GPU processing queue
    bool pushTrade(const Trade& trade);

    // Accessors
    FractalZoomController* getFractalZoomController() { return m_fractalZoomController; }

signals:
    void tradesReady(const std::vector<GPUTypes::Point>& points);
    void candlesReady(const std::vector<CandleUpdate>& candles);
    void aggregatedHeatmapReady(CandleLOD::TimeFrame timeframe, const std::vector<QuadInstance>& bids, const std::vector<QuadInstance>& asks);
    void performanceAlert(const QString& message);

private slots:
    void processIncomingData();
    void processHighFrequencyCandles();   // Process 100ms candles
    void processMediumFrequencyCandles(); // Process 500ms candles
    void processSecondCandles();          // Process 1s candles
    void processHeatmap();                // Process order book data
    
    // 🎯 FRACTAL ZOOM COORDINATION
    void onOrderBookUpdateIntervalChanged(int intervalMs);
    void onTradePointUpdateIntervalChanged(int intervalMs);
    void onOrderBookDepthChanged(size_t maxLevels);
    void onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame);

private:
    // Lock-free queues
    TradeQueue m_tradeQueue;         // 65536 = 2^16 (buffer @ 20M+ ops/s)
    OrderBookQueue m_orderBookQueue; // 16384 = 2^14
    
    // 🚀 ULTRA-FAST: Reference to centralized O(1) Order Book from DataCache
    CoinbaseStreamClient* m_coinbaseClient = nullptr;
    
    // Zero-malloc buffers (pre-allocated, cursor-based)
    std::vector<GPUTypes::Point> m_tradeBuffer;
    std::vector<QuadInstance> m_heatmapBuffer; // 🚨 LEGACY: This will be removed or repurposed
    std::vector<CandleUpdate> m_candleBuffer;
    size_t m_tradeWriteCursor = 0;
    size_t m_heatmapWriteCursor = 0;
    size_t m_candleWriteCursor = 0;
    
    // Configuration
    size_t m_reserveSize;
    int m_firehoseRate = 20000; // Legacy default (actual performance: 20M+ ops/s)
    
    // Performance monitoring
    std::atomic<size_t> m_pointsPushed{0};
    std::atomic<size_t> m_processedTrades{0};
    std::atomic<size_t> m_processedOrderBooks{0};
    
    // Timers
    QTimer* m_processTimer = nullptr;
    QTimer* m_orderBookTimer = nullptr;
    QTimer* m_tradePointTimer = nullptr;
    QTimer* m_highFreqCandleTimer = nullptr;
    QTimer* m_mediumFreqCandleTimer = nullptr;
    QTimer* m_secondCandleTimer = nullptr;
    
    // 🎯 FRACTAL ZOOM CONTROLLER
    FractalZoomController* m_fractalZoomController = nullptr;
    
    // 🔥 ADAPTIVE CONFIGURATION - Changes based on zoom level
    size_t m_currentOrderBookDepth = 1000; // Default depth
    CandleLOD::TimeFrame m_currentTimeFrame = CandleLOD::TF_1min;

    CandleQueue m_candleQueue;
    CandleLOD m_candleLOD;
    std::array<int64_t, CandleLOD::NUM_TIMEFRAMES> m_lastEmittedCandleTime{};
    std::string m_currentSymbol;
    
    // 🔥 NEW: State for the heatmap aggregation engine
    using PriceVolumeMap = std::map<double, double>;
    std::array<PriceVolumeMap, CandleLOD::NUM_TIMEFRAMES> m_bidHeatmapAggregators;
    std::array<PriceVolumeMap, CandleLOD::NUM_TIMEFRAMES> m_askHeatmapAggregators;
    std::array<int64_t, CandleLOD::NUM_TIMEFRAMES> m_heatmapBucketStartTimes;
    
    // Coordinate mapping (cached for performance)
    struct CoordinateCache {
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double timeSpanMs = 60000.0; // 60 seconds
        bool initialized = false;
    } m_coordCache;
    
    // Helper methods
    void initializeBuffers();
    GPUTypes::Point convertTradeToGPUPoint(const Trade& trade);
    void updateCoordinateCache(double price);
    void processCandleTimeFrame(CandleLOD::TimeFrame timeframe);  // Time-based candle processing
    void cleanupOldTradeHistory(); // 🚀 PHASE 2: Time-based history cleanup
    void processHeatmapAggregation(const OrderBook& book); // 🔥 NEW: Core aggregation logic
}; 