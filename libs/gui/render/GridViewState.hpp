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
    void handlePanStart(const QPointF& position);
    void handlePanMove(const QPointF& position);
    void handlePanEnd();
    
    void enableAutoScroll(bool enabled);
    void resetZoom();

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
    
    // Mouse interaction state
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    QPointF m_initialMousePos;
    QPointF m_panVisualOffset;
    QElapsedTimer m_interactionTimer;
};