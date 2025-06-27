#pragma once
#include <QObject>
#include <QTimer>
#include <QSettings>
#include <vector>
#include <atomic>
#include "../core/lockfreequeue.h"
#include "../core/tradedata.h"

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

signals:
    void tradesReady(const GPUTypes::Point* points, size_t count);
    void heatmapReady(const GPUTypes::QuadInstance* quads, size_t count);
    void performanceAlert(const QString& message);

private slots:
    void processIncomingData();

private:
    // Lock-free queues
    TradeQueue m_tradeQueue;         // 65536 = 2^16 (3.3s buffer @ 20k msg/s)
    OrderBookQueue m_orderBookQueue; // 16384 = 2^14
    
    // Zero-malloc buffers (pre-allocated, cursor-based)
    std::vector<GPUTypes::Point> m_tradeBuffer;
    std::vector<GPUTypes::QuadInstance> m_heatmapBuffer;
    size_t m_tradeWriteCursor = 0;
    size_t m_heatmapWriteCursor = 0;
    
    // Configuration
    size_t m_reserveSize;
    int m_firehoseRate = 20000; // Default 20k msg/s, override with --firehose-rate
    
    // Performance monitoring
    std::atomic<size_t> m_pointsPushed{0};
    std::atomic<size_t> m_processedTrades{0};
    std::atomic<int> m_frameDrops{0};
    
    // Processing timer
    QTimer* m_processTimer;
    
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
    void resetWriteCursors();
}; 