/*
Sentinel — GridViewState
Role: A state machine that manages the chart's viewport, handling all pan and zoom logic.
Inputs/Outputs: Takes user input events (pan, zoom); emits viewportChanged signals.
Threading: Lives on the main GUI thread; all methods are executed on this thread.
Performance: Logic is designed for smooth, interactive viewport manipulation.
Integration: Owned by UnifiedGridRenderer; its state dictates the rendered data window.
Observability: Logs pan and zoom actions via sLog_Render for debugging interactions.
Related: GridViewState.cpp, UnifiedGridRenderer.h, CoordinateSystem.h.
Assumptions: Driven by user input events forwarded from a UI component.
*/
#pragma once
#include <QObject>
#include <QPointF>
#include <QMatrix4x4>
#include <QElapsedTimer>

class GridViewState : public QObject {
    Q_OBJECT
    
public:
    explicit GridViewState(QObject* parent = nullptr);
    
    // Viewport bounds
    qint64 getVisibleTimeStart() const { return m_visibleTimeStart_ms; }
    qint64 getVisibleTimeEnd() const { return m_visibleTimeEnd_ms; }
    double getMinPrice() const { return m_minPrice; }
    double getMaxPrice() const { return m_maxPrice; }
    double getViewportWidth() const { return m_viewportWidth; }
    double getViewportHeight() const { return m_viewportHeight; }
    
    // Pan/zoom state
    double getZoomFactor() const { return m_zoomFactor; }
    QPointF getPanVisualOffset() const { return m_panVisualOffset; }
    bool isAutoScrollEnabled() const { return m_autoScrollEnabled; }
    bool isTimeWindowValid() const { return m_timeWindowValid; }
    
    // Viewport management
    void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    void setViewportSize(double width, double height);
    QMatrix4x4 calculateViewportTransform(const QRectF& itemBounds) const;
    
    // Interaction handling
    void handleZoom(double delta, const QPointF& center);
    void handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize);
    void handleZoomWithSensitivity(double rawDelta, const QPointF& center, const QSizeF& viewportSize);
    void handlePanStart(const QPointF& position);
    void handlePanMove(const QPointF& position);
    void handlePanEnd();
    
    // Directional pan methods
    void panLeft();
    void panRight();
    void panUp();
    void panDown();
    
    void enableAutoScroll(bool enabled);
    void resetZoom();
    
    // 🚀 PRICE LOD: Dynamic price resolution based on zoom level
    double calculateOptimalPriceResolution() const;

signals:
    void viewportChanged();
    void panVisualOffsetChanged();
    void autoScrollEnabledChanged();

private:
    // Viewport bounds
    qint64 m_visibleTimeStart_ms = 0;
    qint64 m_visibleTimeEnd_ms = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    bool m_timeWindowValid = false;
    
    // Viewport dimensions for coordinate conversion
    double m_viewportWidth = 800.0;
    double m_viewportHeight = 600.0;
    
    // Pan/zoom state
    bool m_autoScrollEnabled = true;
    double m_zoomFactor = 1.0;
    double m_panOffsetTime_ms = 0.0;
    double m_panOffsetPrice = 0.0;
    
    // Zoom sensitivity control
    static constexpr double ZOOM_SENSITIVITY = 0.0005;
    static constexpr double MAX_ZOOM_DELTA = 0.4;
    
    // Mouse interaction state
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    QPointF m_initialMousePos;
    QPointF m_panVisualOffset;
    QElapsedTimer m_interactionTimer;
};