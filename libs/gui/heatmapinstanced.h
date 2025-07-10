#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include "chartcoordinator.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <array>
#include "../core/tradedata.h"
#include "candlelod.h"

class HeatmapBatched : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    // ======================== THE FIX: START ========================
    // Declare properties with READ, WRITE, and NOTIFY for full QML binding
    Q_PROPERTY(qint64 viewTimeStart READ viewTimeStart WRITE setViewTimeStart NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewTimeEnd READ viewTimeEnd WRITE setViewTimeEnd NOTIFY viewChanged)
    Q_PROPERTY(double viewMinPrice READ viewMinPrice WRITE setViewMinPrice NOTIFY viewChanged)
    Q_PROPERTY(double viewMaxPrice READ viewMaxPrice WRITE setViewMaxPrice NOTIFY viewChanged)
    // ========================= THE FIX: END =========================

public:
    explicit HeatmapBatched(QQuickItem* parent = nullptr);
    ~HeatmapBatched();

    // Configuration
    Q_INVOKABLE void setPriceRange(double minPrice, double maxPrice);
    Q_INVOKABLE void setIntensityScale(double scale);
    
    // Public setter functions (the WRITE accessors)
    void setViewTimeStart(qint64 time);
    void setViewTimeEnd(qint64 time);
    void setViewMinPrice(double price);
    void setViewMaxPrice(double price);

    // Public getter functions (the READ accessors)
    qint64 viewTimeStart() const { return m_visibleTimeStart_ms; }
    qint64 viewTimeEnd() const { return m_visibleTimeEnd_ms; }
    double viewMinPrice() const { return m_minPrice; }
    double viewMaxPrice() const { return m_maxPrice; }
    
    // View update method (internal use)
    void updateView(qint64 startMs, qint64 endMs, double minPrice, double maxPrice);

public slots:
    // 🔥 NEW: Model B architecture slots
    void onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame);
    void onHeatmapDataReady(CandleLOD::TimeFrame timeframe, const std::vector<QuadInstance>& bids, const std::vector<QuadInstance>& asks);

signals: 
    void dataUpdated();
    void viewChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);

private:
    // 🔥 MODEL B: An array of buffers, one for each of the 8 timeframes
    std::array<std::vector<QuadInstance>, CandleLOD::NUM_TIMEFRAMES> m_bidBuffers;
    std::array<std::vector<QuadInstance>, CandleLOD::NUM_TIMEFRAMES> m_askBuffers;
    CandleLOD::TimeFrame m_activeTimeFrame = CandleLOD::TF_1sec; // Default active timeframe

    // Thread-safe buffer management
    std::mutex m_dataMutex;
    std::atomic<bool> m_geometryDirty{true};
    std::atomic<bool> m_bidsDirty{false};
    std::atomic<bool> m_asksDirty{false};
    
    // 🎯 VIEW PARAMETERS - Synchronized from GPUChartWidget
    double m_minPrice = 107000.0;  // Will be updated via setTimeWindow()
    double m_maxPrice = 109000.0;
    double m_intensityScale = 1.0;  // Volume intensity scaling
    
    // 🔥 NEW: Time window for unified coordinate system  
    int64_t m_visibleTimeStart_ms = 0;
    int64_t m_visibleTimeEnd_ms = 0;
    bool m_timeWindowValid = false;
    
    // 🔥 REUSE NODES FOR PERFORMANCE (avoid allocation)
    QSGGeometryNode* m_bidNode = nullptr;   // Green quads node
    QSGGeometryNode* m_askNode = nullptr;   // Red quads node
    
    // Helper methods
    // Unified coordinate transform using ChartCoordinator
    QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    void setChartCoordinator(ChartCoordinator* coordinator);
    ChartCoordinator* m_chartCoordinator = nullptr;
    
    // GPU node creation and updates
    QSGGeometryNode* createQuadGeometryNode(const std::vector<QuadInstance>& instances, 
                                          const QColor& baseColor);
    void updateNodeGeometry(QSGGeometryNode* node,
                          const std::vector<QuadInstance>& instances,
                          const QColor& baseColor);
};