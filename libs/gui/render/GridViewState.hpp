#pragma once
#include <QObject>
#include <QPointF>
#include <QMatrix4x4>
#include <QElapsedTimer>
#include <functional>
#include <utility>

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
    
    QPointF getPanVisualOffset() const { return m_panVisualOffset; }
    bool isAutoScrollEnabled() const { return m_autoScrollEnabled; }
    bool isTimeWindowValid() const { return m_timeWindowValid; }
    bool isDragging() const { return m_isDragging; }
    
    void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    // Spec rules 1, 2 and 9 (S6b, GPU heatmap): optional maximum spans (<= 0: none).
    // setViewport and every zoom handler apply them, so wheel, axis drags and the
    // Agent API clamp the same way; setting them re-clamps the current viewport.
    void setMaxSpans(double maxTimeSpanMs, double maxPriceSpan);
    // New limits and a new viewport as one change (one viewportVersion step): the
    // limits are not applied to the old viewport first. A timeframe switch uses it.
    void setViewportAndMaxSpans(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax,
                                double maxTimeSpanMs, double maxPriceSpan);
    double maxTimeSpanMs() const { return m_maxTimeSpanMs; }
    double maxPriceSpan() const { return m_maxPriceSpan; }
    // Zoom-in floors (<= 0: none; the legacy renderer sets none). Only the zoom
    // handlers apply them: a zoom-in step stops at the minimum span, a direct
    // viewport is not widened. Setting them does not change the viewport.
    void setMinSpans(double minTimeSpanMs, double minPriceSpan);
    double minTimeSpanMs() const { return m_minTimeSpanMs; }
    double minPriceSpan() const { return m_minPriceSpan; }
    void setViewportSize(double width, double height);
    // Auto price scale (docs/research/2026-10-viewport-autoscale.md; off by default,
    // the gpu renderer turns it on). While on, setViewport takes its price range from
    // the price fit for the new time range (so a time change and its refit are ONE
    // viewport change), chart drags and keyboard pans move time only, and the chart
    // wheel zooms time only. A price zoom (axis drag or wheel) turns it off first.
    // The fit returns false when it has nothing to fit (the given price is kept).
    using PriceFit = std::function<bool(qint64 timeStart, qint64 timeEnd, double& priceMin, double& priceMax)>;
    void setPriceFit(PriceFit fit) { m_priceFit = std::move(fit); }
    bool autoPriceScale() const { return m_autoPriceScale; }
    // The time window on screen: the committed one shifted by an active drag's visual
    // offset (a drag commits only at release). The auto price fit and its candle
    // checks use it, so newly revealed candles fit during the drag.
    std::pair<qint64, qint64> displayedTimeWindow() const;
    // Does not touch the viewport (the caller refits through setViewport).
    void setAutoPriceScale(bool enabled);
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
    void autoPriceScaleChanged();
    void priceInteracted();

private:
    // A drag's time shift for a window of spanMs (0 when not dragging).
    qint64 dragShiftMs(qint64 spanMs) const;
    // One zoom step of a span (multiplier > 1 zooms in) inside the limits: a
    // zoom-out never narrows and a zoom-in never widens the current span.
    int64_t zoomedTimeSpan(int64_t current, double zoomMultiplier) const;
    double zoomedPriceSpan(double current, double zoomMultiplier) const;

    qint64 m_visibleTimeStart_ms = 0;
    qint64 m_visibleTimeEnd_ms = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    bool m_timeWindowValid = false;
    
    double m_viewportWidth = 800.0;
    double m_viewportHeight = 600.0;
    
    bool m_autoScrollEnabled = true;
    bool m_autoPriceScale = false;
    PriceFit m_priceFit;
    
    static constexpr double ZOOM_SENSITIVITY = 0.0005;
    static constexpr double MAX_ZOOM_DELTA = 0.4;
    
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    QPointF m_initialMousePos;
    QPointF m_panVisualOffset;
    double m_panRemainderTimeMs = 0.0;
    QElapsedTimer m_interactionTimer;
    uint64_t m_viewportVersion = 1;
    double m_maxTimeSpanMs = 0.0;
    double m_maxPriceSpan = 0.0;
    double m_minTimeSpanMs = 0.0;
    double m_minPriceSpan = 0.0;
};
