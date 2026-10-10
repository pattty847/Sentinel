// GridViewState: viewport pan/zoom math; main GUI thread only.
#include "GridViewState.hpp"
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
}

GridViewState::GridViewState(QObject* parent) 
    : QObject(parent) {
    m_interactionTimer.start();
}

void GridViewState::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax,
                                bool preservePlacement) {
    if (!preservePlacement) ++m_placementVersion;
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
    if (m_timeWindowValid) setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice, true);
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

void GridViewState::setRasterAnchor(double fracX, double fracY) {
    if (std::isfinite(fracX)) m_rasterAnchor.fracX = std::clamp(fracX, 0.0, 1.0);
    if (std::isfinite(fracY)) m_rasterAnchor.fracY = std::clamp(fracY, 0.0, 1.0);
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
