#pragma once
#include <QObject>
#include <QPointF>
#include <QMatrix4x4>
#include <QElapsedTimer>

class GridViewState : public QObject {
    Q_OBJECT
    
public:
    explicit GridViewState(QObject* parent = nullptr);
    
    qint64 getVisibleTimeStart() const { return m_visibleTimeStart_ms; }
    qint64 getVisibleTimeEnd() const { return m_visibleTimeEnd_ms; }
    double getMinPrice() const { return m_minPrice; }
    double getMaxPrice() const { return m_maxPrice; }
    double getViewportWidth() const { return m_viewportWidth; }
    double getViewportHeight() const { return m_viewportHeight; }
    uint64_t getViewportVersion() const { return m_viewportVersion; }
    
    double getZoomFactor() const { return m_zoomFactor; }
    QPointF getPanVisualOffset() const { return m_panVisualOffset; }
    bool isAutoScrollEnabled() const { return m_autoScrollEnabled; }
    bool isTimeWindowValid() const { return m_timeWindowValid; }
    bool isDragging() const { return m_isDragging; }
    
    void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    void setViewportSize(double width, double height);
    QMatrix4x4 calculateViewportTransform(const QRectF& itemBounds) const;
    
    void handleZoom(double delta, const QPointF& center);
    void handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize);
    void handleZoomWithSensitivity(double rawDelta, const QPointF& center, const QSizeF& viewportSize);
    void handleTimeZoomWithSensitivity(double rawDelta, double centerX, double viewportWidth);
    void handlePriceZoomWithSensitivity(double rawDelta, double centerY, double viewportHeight);
    void handlePanStart(const QPointF& position);
    void handlePanMove(const QPointF& position);
    void handlePanEnd(bool applyViewport = true);
    void clearPanVisualOffset();
    
    void panLeft();
    void panRight();
    void panUp();
    void panDown();
    
    void enableAutoScroll(bool enabled);
    void resetZoom();
    
    double calculateOptimalPriceResolution() const;

signals:
    void viewportChanged();
    void panVisualOffsetChanged();
    void autoScrollEnabledChanged();

private:
    qint64 m_visibleTimeStart_ms = 0;
    qint64 m_visibleTimeEnd_ms = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    bool m_timeWindowValid = false;
    
    double m_viewportWidth = 800.0;
    double m_viewportHeight = 600.0;
    
    bool m_autoScrollEnabled = true;
    double m_zoomFactor = 1.0;
    double m_panOffsetTime_ms = 0.0;
    double m_panOffsetPrice = 0.0;
    
    static constexpr double ZOOM_SENSITIVITY = 0.0005;
    static constexpr double MAX_ZOOM_DELTA = 0.4;
    static constexpr double MAX_ZOOM_FACTOR = 500.0;
    
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    QPointF m_initialMousePos;
    QPointF m_panVisualOffset;
    double m_panRemainderTimeMs = 0.0;
    QElapsedTimer m_interactionTimer;
    uint64_t m_viewportVersion = 1;
};
