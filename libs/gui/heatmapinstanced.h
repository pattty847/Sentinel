#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QVariantList>
#include <vector>
#include <mutex>
#include <atomic>
#include "tradedata.h"

class HeatMapInstanced : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit HeatMapInstanced(QQuickItem* parent = nullptr);

    // 🎯 PHASE 2: Order Book Data Interface
    Q_INVOKABLE void updateBids(const QVariantList& bidLevels);
    Q_INVOKABLE void updateAsks(const QVariantList& askLevels);
    Q_INVOKABLE void updateOrderBook(const OrderBook& book);
    Q_INVOKABLE void clearOrderBook();
    
    // Configuration
    Q_INVOKABLE void setMaxQuads(int maxQuads);
    Q_INVOKABLE void setPriceRange(double minPrice, double maxPrice);
    Q_INVOKABLE void setIntensityScale(double scale);
    
    // 🔥 FINAL POLISH: Time window + price range synchronization for unified coordinates
    Q_INVOKABLE void setTimeWindow(int64_t startMs, int64_t endMs, double minPrice, double maxPrice);

public slots:
    // Real-time order book updates
    void onOrderBookUpdated(const OrderBook& book);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);

private:
    // 🔥 PHASE 2: QUAD INSTANCE DATA STRUCTURE
    struct QuadInstance {
        float x, y;           // Position (screen coordinates)
        float width, height;  // Quad dimensions
        float r, g, b, a;     // Color (RGBA)
        float intensity;      // Size/volume intensity
        double price;         // Original price (for sorting/filtering)
        double size;          // Original size (for intensity calculation)
        double timestamp;     // Timestamp for unified coordinate system
        
        // 🔥 FINAL POLISH: Store raw data for per-frame recalculation
        double rawTimestamp = 0.0;
        double rawPrice = 0.0;
    };

    // 🚀 SEPARATE BID/ASK BUFFERS (as specified in plan)
    std::vector<QuadInstance> m_bidInstances;   // Green quads (bids)
    std::vector<QuadInstance> m_askInstances;   // Red quads (asks)
    
    // 🔥 HISTORICAL HEATMAP: Store the full history of all heatmap points for accumulation
    std::vector<QuadInstance> m_allBidInstances;
    std::vector<QuadInstance> m_allAskInstances;
    
    // Thread-safe buffer management
    std::mutex m_dataMutex;
    std::atomic<bool> m_geometryDirty{true};
    std::atomic<bool> m_bidsDirty{false};
    std::atomic<bool> m_asksDirty{false};
    
    // 🎯 VIEW PARAMETERS
    double m_minPrice = 49000.0;
    double m_maxPrice = 51000.0;
    int m_maxQuads = 200000;        // 200k quads max (as per plan target)
    double m_intensityScale = 1.0;  // Volume intensity scaling
    
    // 🔥 NEW: Time window for unified coordinate system  
    int64_t m_visibleTimeStart_ms = 0;
    int64_t m_visibleTimeEnd_ms = 0;
    bool m_timeWindowValid = false;
    
    // Performance optimization
    size_t m_maxBidLevels = 100;    // Top 100 bid levels
    size_t m_maxAskLevels = 100;    // Top 100 ask levels
    
    // 🔥 REUSE NODES FOR PERFORMANCE (avoid allocation)
    QSGGeometryNode* m_bidNode = nullptr;   // Green quads node
    QSGGeometryNode* m_askNode = nullptr;   // Red quads node
    
    // Helper methods
    void convertOrderBookToInstances(const OrderBook& book);
    void convertBidsToInstances(const std::vector<OrderBookLevel>& bids);
    void convertAsksToInstances(const std::vector<OrderBookLevel>& asks);
    void cleanupOldHeatmapPoints(); // 🔥 HISTORICAL HEATMAP: Memory management
    QPointF worldToScreen(double timestamp_ms, double price) const; // 🔥 FINAL POLISH: Per-frame coordinate transformation
    QRectF calculateQuadGeometry(double price, double size) const;
    QColor calculateIntensityColor(double size, bool isBid) const;
    void updateBidGeometry();
    void updateAskGeometry();
    void sortAndLimitLevels();
    
    // GPU node creation
    QSGGeometryNode* createQuadGeometryNode(const std::vector<QuadInstance>& instances, 
                                          const QColor& baseColor);
}; 