#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>  // 🚀 NEW: For per-vertex colors
#include <QVariantList>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include "tradedata.h"

class GPUChartWidget : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    // 🚀 PHASE 4: QML Property for auto-scroll state
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    Q_PROPERTY(bool debugInfoVisible READ debugInfoVisible WRITE setDebugInfoVisible NOTIFY debugInfoVisibleChanged)
    Q_PROPERTY(QString debugInfo READ debugInfo NOTIFY debugInfoChanged)

public:
    explicit GPUChartWidget(QQuickItem* parent = nullptr);

    // 🔥 SIMPLIFIED: Remove test data methods - Option B approach
    // Q_INVOKABLE void setTestPoints(const QVariantList& points);  // REMOVED
    
    // REAL DATA INTERFACE (connects to existing StreamController)
    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void updateOrderBook(const OrderBook& book);
    Q_INVOKABLE void clearTrades();
    
    // 🚀 PHASE 1: Performance Configuration
    Q_INVOKABLE void setMaxPoints(int maxPoints);
    Q_INVOKABLE void setTimeSpan(double timeSpanSeconds);
    Q_INVOKABLE void setPriceRange(double minPrice, double maxPrice);
    
    // 🎯 PHASE 2: Dynamic Price Scaling (NEW)
    Q_INVOKABLE void enableDynamicPriceZoom(bool enabled);
    Q_INVOKABLE void setDynamicPriceRange(double rangeSize);
    Q_INVOKABLE void resetPriceRange();
    
    // 🚀 PHASE 4: Property getters for QML
    bool autoScrollEnabled() const { return m_autoScrollEnabled; }
    void enableAutoScroll(bool enabled);
    
    // 🚀 DEBUG INFO: On-screen viewport debugging
    bool debugInfoVisible() const { return m_debugInfoVisible; }
    void setDebugInfoVisible(bool visible);
    QString debugInfo() const { return m_debugInfoText; }

public slots:
    // 🔥 REAL-TIME DATA INTEGRATION
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(const OrderBook& book);
    
    // 🚀 PHASE 4: PAN/ZOOM CONTROL METHODS
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void centerOnPrice(double price);
    Q_INVOKABLE void centerOnTime(qint64 timestamp);

signals:
    // 🚀 PHASE 4: QML notification signals
    void autoScrollEnabledChanged();
    void zoomFactorChanged(double factor);
    void panOffsetChanged(double x, double y);
    
    // 🔥 GEMINI UNIFICATION: Broadcasts the camera's current time window
    void viewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice); // 🔥 FINAL POLISH: Include price range
    void debugInfoVisibleChanged();
    void debugInfoChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);
    
    // 🚀 PHASE 4: MOUSE/TOUCH INTERACTION EVENTS
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct GPUPoint {
        float x, y;           // Screen coordinates
        float r, g, b, a;     // Color (RGBA)
        float size;           // Point size
        int64_t timestamp_ms; // For aging/cleanup
        
        // 🔥 RAW DATA: Store original values before coordinate transformation
        double rawTimestamp = 0.0;  // Original timestamp for coordinate mapping
        double rawPrice = 0.0;      // Original price for coordinate mapping
    };
    
    // 🔥 PHASE 1: VBO TRIPLE-BUFFERING FOR MAXIMUM PERFORMANCE
    struct GPUBuffer {
        std::vector<GPUPoint> points;
        std::atomic<bool> dirty{false};
        std::atomic<bool> inUse{false};
    };
    
    // Triple-buffered GPU data
    static constexpr int BUFFER_COUNT = 3;
    GPUBuffer m_gpuBuffers[BUFFER_COUNT];
    std::atomic<int> m_writeBuffer{0};
    std::atomic<int> m_readBuffer{1};
    std::mutex m_bufferMutex;
    
    // 🔥 GEMINI FIX: Accumulate all trades for persistent rendering
    std::vector<GPUPoint> m_allRenderPoints;
    
    // REAL DATA STORAGE
    std::vector<Trade> m_recentTrades;
    OrderBook m_currentOrderBook;
    std::mutex m_dataMutex;
    
    // 🎯 VIEW PARAMETERS - Dynamic price range (no hardcoded values)
    double m_minPrice = 107000.0;  // Current BTC range - will be updated dynamically
    double m_maxPrice = 109000.0;
    double m_timeSpanMs = 60000.0; // 60 seconds visible window
    int m_maxPoints = 100000;      // 100K points max (configurable)
    
    // 🔥 NEW: STATELESS TIME WINDOW (Option B approach)
    int64_t m_visibleTimeStart_ms = 0;  // Left edge of chart (oldest visible time)
    int64_t m_visibleTimeEnd_ms = 0;    // Right edge of chart (newest visible time)
    bool m_timeWindowInitialized = false;
    
    // 🎯 PHASE 2: Dynamic Price Scaling (NEW)
    bool m_dynamicPriceZoom = true;        // Enable automatic price zoom
    double m_dynamicRangeSize = 50.0;      // Default $50 range for BTC (adjustable)
    double m_lastTradePrice = 0.0;         // Track most recent price for centering
    int m_priceUpdateCount = 0;            // Count trades for range adjustment frequency
    double m_staticMinPrice = 107000.0;    // Fallback static range - current BTC levels  
    double m_staticMaxPrice = 109000.0;    // Fallback static range - current BTC levels
    
    // 🚀 PHASE 4: PAN/ZOOM INTERACTION STATE
    double m_zoomFactor = 1.0;             // Zoom level (1.0 = no zoom)
    double m_panOffsetX = 0.0;             // Pan offset in time (milliseconds)
    double m_panOffsetY = 0.0;             // Pan offset in price
    bool m_autoScrollEnabled = true;       // Auto-scroll to follow latest data
    bool m_isDragging = false;             // Mouse drag state
    QPointF m_lastMousePos;                // Last mouse position for drag delta
    QElapsedTimer m_interactionTimer;      // Measure interaction latency
    
    // 🎯 PHASE 4: Zoom constraints and smooth interaction
    static constexpr double MIN_ZOOM = 0.1;    // 10x zoom out
    static constexpr double MAX_ZOOM = 100.0;  // 100x zoom in
    static constexpr double ZOOM_FACTOR = 1.15; // 15% per wheel step
    
    // 🚀 PERFORMANCE OPTIMIZATION FLAGS
    std::atomic<bool> m_geometryDirty{true};
    std::atomic<bool> m_enableVBO{true};      // VBO optimization
    
    // 🟡 PRICE CHANGE TRACKING: For three-color system (uptick/downtick/no-change)
    double m_previousTradePrice = 0.0;        // Track previous price for color logic
    bool m_hasPreviousPrice = false;          // Flag to skip first trade color logic
    
    // 🚀 DEBUG INFO: On-screen display
    bool m_debugInfoVisible = true;  // Show debug info by default
    QString m_debugInfoText;
    
    // 🚀 PHASE 4: PCIe UPLOAD PROFILER
    mutable size_t m_bytesUploadedThisFrame = 0;
    mutable double m_mbPerFrame = 0.0;
    mutable std::atomic<size_t> m_totalBytesUploaded{0};
    mutable std::atomic<int> m_bandwidthWarnings{0};
    static constexpr double PCIE_BUDGET_MB_PER_SECOND = 200.0; // PCIe 3.0 x4 budget
    
    // Helper methods
    void convertTradeToGPUPoint(const Trade& trade, GPUPoint& point);
    void cleanupOldTrades();
    void swapBuffers();
    void updateDynamicPriceRange(double newPrice);
    bool isInCurrentPriceRange(double price) const;
    void updateDebugInfo();
    
    // 🔥 NEW: STATELESS COORDINATE SYSTEM (Option B)
    QPointF worldToScreen(int64_t timestamp_ms, double price) const;
    QPointF screenToWorld(const QPointF& screenPos) const;
    void updateTimeWindow(int64_t newTimestamp);
    void initializeTimeWindow(int64_t firstTimestamp);
    
    // 🎯 PHASE 2: Dynamic Price Scaling (NEW)
    QColor determineDominantTradeColor(const std::vector<GPUPoint>& points) const;
    
    // 🎯 PHASE 4: Pan/Zoom Coordinate Transformation
    void applyPanZoomToPoint(GPUPoint& point, double rawTimestamp, double rawPrice);
    void updateAutoScroll();
    double calculateInteractionLatency() const;
    
    // 🔥 PHASE 1: VBO Management
    void appendTradeToVBO(const Trade& trade);

    // 🔥 NEW: Share time window with heatmap for unified coordinates
    int64_t getVisibleTimeStart() const { return m_visibleTimeStart_ms; }
    int64_t getVisibleTimeEnd() const { return m_visibleTimeEnd_ms; }
    bool isTimeWindowInitialized() const { return m_timeWindowInitialized; }
}; 