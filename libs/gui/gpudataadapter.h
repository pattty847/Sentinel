#pragma once
#include <QObject>
#include <QTimer>
#include <QSettings>
#include <vector>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <chrono>
#include "../core/lockfreequeue.h"
#include "../core/tradedata.h"
#include "candlelod.h"

// Forward declarations for GPU components
namespace GPUTypes {
    struct Point {
        float x, y, r, g, b, a;
    };
    
    struct QuadInstance {
        float x, y, width, height;
        float r, g, b, a;
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
    
    // CLI flag: --firehose-rate for QA sweep testing
    void setFirehoseRate(int msgsPerSec) { m_firehoseRate = msgsPerSec; }
    
    // Lock-free push from WebSocket thread
    bool pushTrade(const Trade& trade);
    bool pushOrderBook(const OrderBook& orderBook);
    
    // Performance monitoring
    size_t getPointsThroughput() const { return m_pointsPushed.load(); }
    size_t getProcessedTrades() const { return m_processedTrades.load(); }
    bool hasDroppedFrames() const { return m_frameDrops.load() > 0; }
    
    // Configuration
    void setReserveSize(size_t size);
    size_t getReserveSize() const { return m_reserveSize; }
    
    // 🚀 O(1) Order Book Access
    const FastOrderBook& getFastOrderBook() const { return m_fastOrderBook; }
    double getBestBid() const { return m_fastOrderBook.getBestBidPrice(); }
    double getBestAsk() const { return m_fastOrderBook.getBestAskPrice(); }
    double getSpread() const { return m_fastOrderBook.getSpread(); }

signals:
    void tradesReady(const GPUTypes::Point* points, size_t count);
    void heatmapReady(const GPUTypes::QuadInstance* quads, size_t count);
    void candlesReady(const std::vector<CandleUpdate>& candles);
    void performanceAlert(const QString& message);

private slots:
    void processIncomingData();           // Process trades + order books (16ms)
    void processHighFrequencyCandles();   // Process 100ms candles
    void processMediumFrequencyCandles(); // Process 500ms candles
    void processSecondCandles();          // Process 1s candles

private:
    // Lock-free queues
    TradeQueue m_tradeQueue;         // 65536 = 2^16 (buffer @ 20M+ ops/s)
    OrderBookQueue m_orderBookQueue; // 16384 = 2^14
    
    // 🚀 ULTRA-FAST: O(1) Order Book for HFT Performance  
    FastOrderBook m_fastOrderBook;
    
    // Zero-malloc buffers (pre-allocated, cursor-based)
    std::vector<GPUTypes::Point> m_tradeBuffer;
    std::vector<GPUTypes::QuadInstance> m_heatmapBuffer;
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
    std::atomic<int> m_frameDrops{0};
    
    // Processing timers
    QTimer* m_processTimer;      // 16ms - Trade scatter + Order book heatmap
    QTimer* m_candle100msTimer;  // 100ms - High-frequency candles
    QTimer* m_candle500msTimer;  // 500ms - Medium-frequency candles  
    QTimer* m_candle1sTimer;     // 1000ms - Second candles

    CandleQueue m_candleQueue;
    CandleLOD m_candleLOD;
    std::array<int64_t, CandleLOD::NUM_TIMEFRAMES> m_lastEmittedCandleTime{};
    std::string m_currentSymbol;
    
    // Coordinate mapping (cached for performance)
    struct CoordinateCache {
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double timeSpanMs = 60000.0; // 60 seconds
        bool initialized = false;
    } m_coordCache;
    
    // 🚀 PHASE 2: TIME-WINDOWED TRADE HISTORY BUFFER
    // Maintains rolling 10-minute window of trades for re-aggregation
    std::deque<Trade> m_tradeHistory;
    std::mutex m_tradeHistoryMutex;
    static constexpr std::chrono::seconds HISTORY_WINDOW_SPAN{600}; // 10 minutes
    int64_t m_lastHistoryCleanup_ms = 0;
    static constexpr int64_t CLEANUP_INTERVAL_MS = 5000; // Clean up every 5 seconds
    
    // Helper methods
    void initializeBuffers();
    GPUTypes::Point convertTradeToGPUPoint(const Trade& trade);
    void updateCoordinateCache(double price);
    void resetWriteCursors();
    void processCandleTimeFrame(CandleLOD::TimeFrame timeframe);  // Time-based candle processing
    void cleanupOldTradeHistory(); // 🚀 PHASE 2: Time-based history cleanup
    void convertFastOrderBookToQuads(); // 🚀 Convert O(1) order book to GPU quads
}; 