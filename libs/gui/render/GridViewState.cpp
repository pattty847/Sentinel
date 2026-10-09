// GridViewState: viewport pan/zoom math; main GUI thread only.
#include "GridViewState.hpp"
#include <QSizeF>
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
// Without a maximum (legacy renderer) a zoom-out still stops somewhere finite:
// 50 years keeps every ms computation far from int64 overflow.
constexpr int64_t kSaneTimeSpanMs = 50LL * 365 * 24 * 3600 * 1000;
// Largest whole-ms time span allowed by maxSpan (<= 0: no limit but the sane one).
int64_t timeSpanLimit(double maxSpan) {
    return maxSpan > 0 ? std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxSpan))) : kSaneTimeSpanMs;
}
double clampProcessedZoomDelta(double rawDelta, double sensitivity, double maxDelta) {
    const double processed = rawDelta * sensitivity;
    return std::max(-maxDelta, std::min(maxDelta, processed));
}
}

GridViewState::GridViewState(QObject* parent) 
    : QObject(parent) {
    m_interactionTimer.start();
}

void GridViewState::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) {
    // Spec rules 1 and 9: a span over the limit shrinks about its centre. The zoom
    // handlers clamp about their anchor first, so this only acts on direct calls.
    const int64_t timeLimit = timeSpanLimit(m_maxTimeSpanMs);
    if (timeEnd - timeStart > timeLimit) {
        const double centre = (static_cast<double>(timeStart) + static_cast<double>(timeEnd)) * 0.5;
        timeStart = static_cast<qint64>(std::llround(centre - static_cast<double>(timeLimit) * 0.5));
        timeEnd = timeStart + timeLimit;
    }
    if (m_autoPriceScale && m_priceFit) {
        // Auto price scale: the price range follows the new time range, in this change
        // (during a drag, the range on screen).
        double lo = priceMin, hi = priceMax;
        const qint64 shift = dragShiftMs(timeEnd - timeStart);
        if (m_priceFit(timeStart + shift, timeEnd + shift, lo, hi) && std::isfinite(lo) && std::isfinite(hi) &&
            hi > lo) {
            priceMin = lo;
            priceMax = hi;
        }
    }
    if (m_maxPriceSpan > 0 && priceMax - priceMin > m_maxPriceSpan) {
        const double centre = (priceMin + priceMax) * 0.5;
        priceMin = centre - m_maxPriceSpan * 0.5;
        priceMax = centre + m_maxPriceSpan * 0.5;
    }
    bool changed = false;
    if (m_visibleTimeStart_ms != timeStart) {
        m_visibleTimeStart_ms = timeStart;
        changed = true;
    }
    
    if (m_visibleTimeEnd_ms != timeEnd) {
        m_visibleTimeEnd_ms = timeEnd;
        changed = true;
    }
    
    if (m_minPrice != priceMin) {
        m_minPrice = priceMin;
        changed = true;
    }
    
    if (m_maxPrice != priceMax) {
        m_maxPrice = priceMax;
        changed = true;
    }
    
    m_timeWindowValid = true;
    if (changed) {
        ++m_viewportVersion;
        sLog_Probe("viewport.set", "time=[" << timeStart << ".." << timeEnd << "]"
                   << " price=[" << priceMin << ".." << priceMax << "]"
                   << " version=" << m_viewportVersion);
        emit viewportChanged();
    }
}

void GridViewState::setMaxSpans(double maxTimeSpanMs, double maxPriceSpan) {
    const double time = std::isfinite(maxTimeSpanMs) && maxTimeSpanMs > 0 ? maxTimeSpanMs : 0.0;
    const double price = std::isfinite(maxPriceSpan) && maxPriceSpan > 0 ? maxPriceSpan : 0.0;
    if (time == m_maxTimeSpanMs && price == m_maxPriceSpan) return;
    m_maxTimeSpanMs = time;
    m_maxPriceSpan = price;
    if (m_timeWindowValid) setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
}

void GridViewState::setMinSpans(double minTimeSpanMs, double minPriceSpan) {
    m_minTimeSpanMs = std::isfinite(minTimeSpanMs) && minTimeSpanMs > 0 ? minTimeSpanMs : 0.0;
    m_minPriceSpan = std::isfinite(minPriceSpan) && minPriceSpan > 0 ? minPriceSpan : 0.0;
}

void GridViewState::setViewportAndMaxSpans(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax,
                                           double maxTimeSpanMs, double maxPriceSpan) {
    m_maxTimeSpanMs = std::isfinite(maxTimeSpanMs) && maxTimeSpanMs > 0 ? maxTimeSpanMs : 0.0;
    m_maxPriceSpan = std::isfinite(maxPriceSpan) && maxPriceSpan > 0 ? maxPriceSpan : 0.0;
    setViewport(timeStart, timeEnd, priceMin, priceMax); // clamps to the new limits
}

qint64 GridViewState::dragShiftMs(qint64 spanMs) const {
    if (!m_isDragging || m_panVisualOffset.x() == 0.0) return 0;
    qint64 timeShift = 0;
    double priceShift = 0.0;
    if (m_panShift && m_panShift(QPointF(m_panVisualOffset.x(), 0.0), timeShift, priceShift)) return timeShift;
    if (m_viewportWidth <= 0) return 0;
    return static_cast<qint64>(std::floor(-m_panVisualOffset.x() * static_cast<double>(spanMs) / m_viewportWidth));
}

std::pair<qint64, qint64> GridViewState::displayedTimeWindow() const {
    const qint64 shift = dragShiftMs(m_visibleTimeEnd_ms - m_visibleTimeStart_ms);
    return {m_visibleTimeStart_ms + shift, m_visibleTimeEnd_ms + shift};
}

void GridViewState::setAutoPriceScale(bool enabled) {
    if (m_autoPriceScale == enabled) return;
    m_autoPriceScale = enabled;
    emit autoPriceScaleChanged();
}

void GridViewState::setViewportSize(double width, double height) {
    if (width > 0 && height > 0) {
        m_viewportWidth = width;
        m_viewportHeight = height;
        ++m_viewportVersion;
    }
}

std::tuple<double, double, bool> GridViewState::zoomAnchor(double fracX, double fracY, double continuousTime,
                                                          double continuousPrice) const {
    double time = continuousTime, price = continuousPrice;
    if (!m_isDragging && m_drawnPoint && m_drawnPoint(fracX, fracY, time, price) && std::isfinite(time) &&
        std::isfinite(price))
        return {time, price, true};
    return {continuousTime, continuousPrice, false};
}

void GridViewState::setRasterAnchor(double fracX, double fracY) {
    if (std::isfinite(fracX)) m_rasterAnchor.fracX = std::clamp(fracX, 0.0, 1.0);
    if (std::isfinite(fracY)) m_rasterAnchor.fracY = std::clamp(fracY, 0.0, 1.0);
}

void GridViewState::handleZoom(double delta, const QPointF& center) {
    handleZoomWithViewport(delta, center, QSizeF(m_viewportWidth, m_viewportHeight));
}

void GridViewState::handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    if (center.x() < 0 || center.y() < 0) return;

    const double clampedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, delta));
    const double zoomMultiplier = 1.0 + clampedDelta;
    if (zoomMultiplier <= 0.0 || zoomMultiplier == 1.0) return;
    const int64_t currentTimeRange = std::max<int64_t>(1, m_visibleTimeEnd_ms - m_visibleTimeStart_ms);
    const double currentPriceRange = m_maxPrice > m_minPrice ? m_maxPrice - m_minPrice : 1.0;
    // Spec rules 1, 2 and 9: the spans themselves are zoomed and clamped (no relative
    // zoom factor: an axis zoom that widened the view cannot leave the wheel stuck).
    // Auto price scale: the wheel zooms time only (the price fit follows the candles).
    const int64_t newTimeRange = zoomedTimeSpan(currentTimeRange, zoomMultiplier);
    const double newPriceRange = m_autoPriceScale ? currentPriceRange : zoomedPriceSpan(currentPriceRange, zoomMultiplier);
    if (newTimeRange == currentTimeRange && newPriceRange == currentPriceRange) return; // at the limits

    double centerTimeRatio = center.x() / viewportSize.width();
    double centerPriceRatio = 1.0 - (center.y() / viewportSize.height());
    centerTimeRatio = std::max(0.0, std::min(1.0, centerTimeRatio));
    centerPriceRatio = std::max(0.0, std::min(1.0, centerPriceRatio));
    // The content under the cursor stays put: an axis this event zooms is solved about
    // what is drawn there (the raster camera before this zoom) and the camera re-snaps
    // about the cursor. An axis it does not zoom (the auto price scale's price, a span at
    // its limit) keeps its bounds and its anchor exactly: re-solving it about the drawn
    // point would translate it.
    const bool zoomTime = newTimeRange != currentTimeRange, zoomPrice = newPriceRange != currentPriceRange;
    const auto [currentCenterTime, currentCenterPrice, drawn] =
        zoomAnchor(centerTimeRatio, 1.0 - centerPriceRatio,
                   static_cast<double>(m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio)),
                   m_minPrice + (currentPriceRange * centerPriceRatio));
    setRasterAnchor(zoomTime ? centerTimeRatio : m_rasterAnchor.fracX,
                    zoomPrice ? 1.0 - centerPriceRatio : m_rasterAnchor.fracY);
    const double newStartD = currentCenterTime - static_cast<double>(newTimeRange) * centerTimeRatio;
    const int64_t newTimeStart = !zoomTime ? m_visibleTimeStart_ms
                                           : static_cast<int64_t>(drawn ? std::llround(newStartD) : std::floor(newStartD));
    const int64_t newTimeEnd = newTimeStart + newTimeRange;
    const double newMinPrice = zoomPrice ? currentCenterPrice - (newPriceRange * centerPriceRatio) : m_minPrice;
    const double newMaxPrice = zoomPrice ? currentCenterPrice + (newPriceRange * (1.0 - centerPriceRatio)) : m_maxPrice;

    sLog_Probe("viewport.zoom", "delta=" << delta << "->" << clampedDelta
               << " mouse=(" << center.x() << "," << center.y() << ")"
               << " centerRatio=(" << centerTimeRatio << "," << centerPriceRatio << ")"
               << " time=[" << m_visibleTimeStart_ms << ".." << m_visibleTimeEnd_ms << "]->["
               << newTimeStart << ".." << newTimeEnd << "]"
               << " price=[" << m_minPrice << ".." << m_maxPrice << "]->["
               << newMinPrice << ".." << newMaxPrice << "]");
    if (newTimeEnd <= newTimeStart || !(newMaxPrice > newMinPrice)) {
        sLog_Warning("Zoom aborted: invalid final bounds time=[" << newTimeStart << ".." << newTimeEnd
                     << "] price=[" << newMinPrice << ".." << newMaxPrice << "]");
        return;
    }
    setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice);
    if (!m_autoPriceScale) emit priceInteracted();
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        emit autoScrollEnabledChanged();
    }
}

int64_t GridViewState::zoomedTimeSpan(int64_t current, double zoomMultiplier) const {
    const double wanted = static_cast<double>(current) / zoomMultiplier;
    const bool zoomingOut = zoomMultiplier < 1.0;
    int64_t span = std::max<int64_t>(1, static_cast<int64_t>(zoomingOut ? std::ceil(wanted) : std::floor(wanted)));
    if (zoomingOut) {
        span = std::min(span, std::max(current, timeSpanLimit(m_maxTimeSpanMs)));
    } else if (m_minTimeSpanMs > 0) {
        // A zoom-in stops at the floor; a view already below it is not widened.
        const int64_t floorSpan = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(m_minTimeSpanMs)));
        span = std::max(span, std::min(current, floorSpan));
    }
    return span;
}

double GridViewState::zoomedPriceSpan(double current, double zoomMultiplier) const {
    double span = std::max(1e-6, current / zoomMultiplier);
    if (zoomMultiplier < 1.0) {
        if (m_maxPriceSpan > 0) span = std::min(span, std::max(current, m_maxPriceSpan));
    } else if (m_minPriceSpan > 0) {
        span = std::max(span, std::min(current, m_minPriceSpan));
    }
    return span;
}

void GridViewState::handlePanStart(const QPointF& position) {
    m_isDragging = true;
    m_lastMousePos = position;
    m_initialMousePos = position;
    m_panVisualOffset = QPointF(0, 0);
    m_panRemainderTimeMs = 0.0;
}

void GridViewState::handlePanMove(const QPointF& position) {
    if (!m_isDragging) return;
    
    QPointF delta = position - m_lastMousePos;
    if (m_autoPriceScale) delta.setY(0.0); // auto price scale: no vertical pan
    m_panVisualOffset += delta;
    m_lastMousePos = position;
    
    emit panVisualOffsetChanged();
    // Auto price scale: the price follows the candles the drag reveals (TradingView),
    // a viewport change only when the fit for the displayed window changed.
    if (m_autoPriceScale && m_priceFit && m_timeWindowValid && delta.x() != 0.0) {
        QElapsedTimer cost;
        cost.start();
        const uint64_t version = m_viewportVersion;
        setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
        sLog_Probe("viewport.dragfit", "us=" << cost.nsecsElapsed() / 1000.0 << " bumped=" << (m_viewportVersion != version));
    }
}

void GridViewState::handlePanEnd(bool applyViewport) {
    if (!m_isDragging) return;

    m_isDragging = false;
    if (!applyViewport) {
        // A cancelled drag: the time stays, so the price fits the committed window again.
        if (m_autoPriceScale && m_priceFit && m_timeWindowValid)
            setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
        return;
    }
    // Raster camera: commit exactly what the picture shows (whole device pixels at
    // the drawn scale), however small: a release never moves the picture.
    qint64 timeShift = 0;
    double priceShift = 0.0;
    if (m_panShift && m_timeWindowValid && m_panShift(m_panVisualOffset, timeShift, priceShift)) {
        if (timeShift != 0 || priceShift != 0.0) {
            setViewport(m_visibleTimeStart_ms + timeShift, m_visibleTimeEnd_ms + timeShift, m_minPrice + priceShift,
                        m_maxPrice + priceShift);
            if (priceShift != 0.0) emit priceInteracted();
        }
        return;
    }
    const double threshold = 1.0;
    double timeDeltaF = 0.0;
    double priceDelta = 0.0;
    if (m_panVisualOffset.manhattanLength() > threshold && m_viewportWidth > 0 && m_viewportHeight > 0) {
        int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        double priceRange = m_maxPrice - m_minPrice;
        
        double timePixelsToMs = static_cast<double>(timeRange) / m_viewportWidth;
        double pricePixelsToUnits = priceRange / m_viewportHeight;
        
        timeDeltaF = (-m_panVisualOffset.x() * timePixelsToMs) + m_panRemainderTimeMs;
        const int64_t timeDelta = static_cast<int64_t>(std::floor(timeDeltaF));
        m_panRemainderTimeMs = timeDeltaF - static_cast<double>(timeDelta);
        priceDelta = m_panVisualOffset.y() * pricePixelsToUnits;
        setViewport(m_visibleTimeStart_ms + timeDelta,
                   m_visibleTimeEnd_ms + timeDelta,
                   m_minPrice + priceDelta,
                   m_maxPrice + priceDelta);
        if (priceDelta != 0.0) emit priceInteracted();
    }
}

void GridViewState::handleZoomWithSensitivity(double rawDelta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    double processedDelta = clampProcessedZoomDelta(rawDelta, ZOOM_SENSITIVITY, MAX_ZOOM_DELTA);
    handleZoomWithViewport(processedDelta, center, viewportSize);
}

void GridViewState::handleTimeZoomWithSensitivity(double rawDelta, double centerX, double viewportWidth) {
    if (!m_timeWindowValid || viewportWidth <= 0.0) {
        return;
    }

    const double processedDelta =
        clampProcessedZoomDelta(rawDelta, ZOOM_SENSITIVITY, MAX_ZOOM_DELTA);
    const double zoomMultiplier = 1.0 + processedDelta;
    if (zoomMultiplier <= 0.0 || zoomMultiplier == 1.0) {
        return;
    }

    const int64_t currentTimeRange = std::max<int64_t>(1, m_visibleTimeEnd_ms - m_visibleTimeStart_ms);
    const int64_t newTimeRange = zoomedTimeSpan(currentTimeRange, zoomMultiplier);
    if (newTimeRange == currentTimeRange) return; // at the limit

    double centerRatio = centerX / viewportWidth;
    centerRatio = std::max(0.0, std::min(1.0, centerRatio));
    // About the time drawn under the cursor (raster camera before this zoom).
    const auto [currentCenterTime, unusedPrice, drawn] =
        zoomAnchor(centerRatio, m_rasterAnchor.fracY,
                   static_cast<double>(m_visibleTimeStart_ms +
                                       static_cast<int64_t>(static_cast<double>(currentTimeRange) * centerRatio)),
                   0.0);
    setRasterAnchor(centerRatio, m_rasterAnchor.fracY);
    const double newTimeStartD = currentCenterTime - static_cast<double>(newTimeRange) * centerRatio;
    const int64_t newTimeStart = static_cast<int64_t>(drawn ? std::llround(newTimeStartD) : std::floor(newTimeStartD));
    const int64_t newTimeEnd = newTimeStart + newTimeRange;

    setViewport(newTimeStart, newTimeEnd, m_minPrice, m_maxPrice);
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::handlePriceZoomWithSensitivity(double rawDelta, double centerY, double viewportHeight) {
    if (!m_timeWindowValid || viewportHeight <= 0.0) {
        return;
    }

    const double processedDelta =
        clampProcessedZoomDelta(rawDelta, ZOOM_SENSITIVITY, MAX_ZOOM_DELTA);
    const double zoomMultiplier = 1.0 + processedDelta;
    if (zoomMultiplier <= 0.0 || zoomMultiplier == 1.0) {
        return;
    }

    // A price zoom takes price over: auto price scale off before the viewport moves
    // (else the refit in setViewport would undo the zoom).
    setAutoPriceScale(false);
    const double currentPriceRange = m_maxPrice > m_minPrice ? m_maxPrice - m_minPrice : 1.0;
    const double newPriceRange = zoomedPriceSpan(currentPriceRange, zoomMultiplier);
    if (newPriceRange == currentPriceRange) return; // at the limit

    double centerRatio = 1.0 - (centerY / viewportHeight);
    centerRatio = std::max(0.0, std::min(1.0, centerRatio));
    // About the price drawn under the cursor (raster camera before this zoom).
    const auto [unusedTime, currentCenterPrice, drawn] =
        zoomAnchor(m_rasterAnchor.fracX, 1.0 - centerRatio, 0.0, m_minPrice + currentPriceRange * centerRatio);
    (void)drawn;
    setRasterAnchor(m_rasterAnchor.fracX, 1.0 - centerRatio);
    const double newMinPrice = currentCenterPrice - newPriceRange * centerRatio;
    const double newMaxPrice =
        currentCenterPrice + newPriceRange * (1.0 - centerRatio);
    if (newMaxPrice <= newMinPrice) {
        return;
    }

    setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, newMinPrice, newMaxPrice);
    emit priceInteracted();
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::enableAutoScroll(bool enabled) {
    // Following live anchors the raster camera at the live edge (the view end), and
    // it stays there when a drag or a zoom turns following off (no jump).
    if (enabled) setRasterAnchor(1.0, m_rasterAnchor.fracY);
    if (m_autoScrollEnabled != enabled) {
        m_autoScrollEnabled = enabled;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::resetZoom() {
    // Zoom is the viewport's spans (no relative factor to reset): this only drops a
    // pending drag offset.
    m_panVisualOffset = QPointF(0, 0);
    
    emit viewportChanged();
    emit panVisualOffsetChanged();
}

void GridViewState::clearPanVisualOffset() {
    if (!m_panVisualOffset.isNull()) {
        m_panVisualOffset = QPointF(0, 0);
        emit panVisualOffsetChanged();
    }
}

void GridViewState::panLeft() {
    if (!m_timeWindowValid) return;
    // Fixed 10% steps keep keyboard pan predictable across zoom levels.
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1;
    setViewport(
        m_visibleTimeStart_ms - panAmount,
        m_visibleTimeEnd_ms - panAmount,
        m_minPrice,
        m_maxPrice
    );
}

void GridViewState::panRight() {
    if (!m_timeWindowValid) return;
    // Fixed 10% steps keep keyboard pan predictable across zoom levels.
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1;
    setViewport(
        m_visibleTimeStart_ms + panAmount,
        m_visibleTimeEnd_ms + panAmount,
        m_minPrice,
        m_maxPrice
    );
}

void GridViewState::panUp() {
    if (!m_timeWindowValid || m_autoPriceScale) return; // auto price scale: no vertical pan
    // Fixed 10% steps keep keyboard pan predictable across zoom levels.
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1;
    setViewport(
        m_visibleTimeStart_ms,
        m_visibleTimeEnd_ms,
        m_minPrice + panAmount,
        m_maxPrice + panAmount
    );
    emit priceInteracted();
}

void GridViewState::panDown() {
    if (!m_timeWindowValid || m_autoPriceScale) return; // auto price scale: no vertical pan
    // Fixed 10% steps keep keyboard pan predictable across zoom levels.
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1;
    setViewport(
        m_visibleTimeStart_ms,
        m_visibleTimeEnd_ms,
        m_minPrice - panAmount,
        m_maxPrice - panAmount
    );
    emit priceInteracted();
}

double GridViewState::calculateOptimalPriceResolution() const {
    if (!m_timeWindowValid) return 1.0;
    double priceSpan = m_maxPrice - m_minPrice;
    // Bucket sizes tuned for stable label density across zoom.
    if (priceSpan > 500) return 25.0;
    if (priceSpan > 100) return 5.0;
    if (priceSpan > 50) return 1.0;
    if (priceSpan > 10) return 0.50;
    return 0.25;
}
