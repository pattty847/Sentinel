// GridViewState: viewport pan/zoom math; main GUI thread only.
#include "GridViewState.hpp"
#include <QMatrix4x4>
#include <QSizeF>
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
// Largest whole-ms time span allowed by maxSpan (<= 0: no limit).
int64_t timeSpanLimit(double maxSpan) {
    return maxSpan > 0 ? std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxSpan))) : INT64_MAX;
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

void GridViewState::setViewportAndMaxSpans(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax,
                                           double maxTimeSpanMs, double maxPriceSpan) {
    m_maxTimeSpanMs = std::isfinite(maxTimeSpanMs) && maxTimeSpanMs > 0 ? maxTimeSpanMs : 0.0;
    m_maxPriceSpan = std::isfinite(maxPriceSpan) && maxPriceSpan > 0 ? maxPriceSpan : 0.0;
    setViewport(timeStart, timeEnd, priceMin, priceMax); // clamps to the new limits
}

void GridViewState::setViewportSize(double width, double height) {
    if (width > 0 && height > 0) {
        m_viewportWidth = width;
        m_viewportHeight = height;
        ++m_viewportVersion;
    }
}

QMatrix4x4 GridViewState::calculateViewportTransform(const QRectF& itemBounds) const {
    if (!m_timeWindowValid || itemBounds.isEmpty()) {
        return QMatrix4x4();
    }
    const double timeRange = static_cast<double>(m_visibleTimeEnd_ms - m_visibleTimeStart_ms);
    const double priceRange = (m_maxPrice - m_minPrice);
    if (timeRange <= 0.0 || priceRange <= 0.0 || m_viewportWidth <= 0.0 || m_viewportHeight <= 0.0) {
        return QMatrix4x4();
    }

    const double sx = m_viewportWidth / timeRange;
    const double sy = -m_viewportHeight / priceRange;
    QMatrix4x4 transform;
    transform.scale(sx, sy, 1.0);
    transform.translate(-static_cast<double>(m_visibleTimeStart_ms), -m_maxPrice, 0.0);
    if (!m_panVisualOffset.isNull()) {
        QMatrix4x4 screenSpace;
        screenSpace.translate(m_panVisualOffset.x(), m_panVisualOffset.y());
        transform = screenSpace * transform;
    }

    return transform;
}

void GridViewState::handleZoom(double delta, const QPointF& center) {
    handleZoomWithViewport(delta, center, QSizeF(m_viewportWidth, m_viewportHeight));
}

void GridViewState::handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;

    double clampedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, delta));
    double zoomMultiplier = 1.0 + clampedDelta;
    double newZoom = m_zoomFactor * zoomMultiplier;
    newZoom = std::max(0.1, std::min(MAX_ZOOM_FACTOR, newZoom));
    bool clamped = false;
    if (newZoom != m_zoomFactor) {
        if (center.x() >= 0 && center.y() >= 0) {
            int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            double currentPriceRange = m_maxPrice - m_minPrice;
            if (currentTimeRange <= 0) {
                currentTimeRange = 1;
            }
            if (currentPriceRange <= 0.0) {
                currentPriceRange = 1.0;
            }
            const double timeRangeD = static_cast<double>(currentTimeRange) * (m_zoomFactor / newZoom);
            const bool zoomingOut = (newZoom < m_zoomFactor);
            const int64_t wantedTimeRange = std::max<int64_t>(
                1,
                static_cast<int64_t>(zoomingOut ? std::ceil(timeRangeD) : std::floor(timeRangeD))
            );
            const double wantedPriceRange = std::max(1e-6, currentPriceRange * (m_zoomFactor / newZoom));
            // Spec rules 1, 2 and 9: the clamps hold about the anchor.
            const int64_t newTimeRange = std::min(wantedTimeRange, timeSpanLimit(m_maxTimeSpanMs));
            const double newPriceRange = m_maxPriceSpan > 0 ? std::min(wantedPriceRange, m_maxPriceSpan)
                                                            : wantedPriceRange;
            clamped = newTimeRange < wantedTimeRange || newPriceRange < wantedPriceRange;
            if (newTimeRange <= 0 || newPriceRange <= 0.0) {
                sLog_Warning("Zoom aborted: invalid range timeRange=" << newTimeRange
                             << " priceRange=" << newPriceRange << " zoom=" << m_zoomFactor << "->" << newZoom);
                return;
            }
            double centerTimeRatio = center.x() / viewportSize.width();
            double centerPriceRatio = 1.0 - (center.y() / viewportSize.height());
            centerTimeRatio = std::max(0.0, std::min(1.0, centerTimeRatio));
            centerPriceRatio = std::max(0.0, std::min(1.0, centerPriceRatio));

            int64_t currentCenterTime = m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio);
            double currentCenterPrice = m_minPrice + (currentPriceRange * centerPriceRatio);
            const double newTimeRangeD = static_cast<double>(newTimeRange);
            const double newTimeStartD = static_cast<double>(currentCenterTime) - (newTimeRangeD * centerTimeRatio);
            const double newTimeEndD = newTimeStartD + newTimeRangeD;

            int64_t newTimeStart = static_cast<int64_t>(std::floor(newTimeStartD));
            int64_t newTimeEnd = static_cast<int64_t>(std::ceil(newTimeEndD));
            if (newTimeEnd <= newTimeStart) {
                newTimeEnd = newTimeStart + 1;
            }

            double newMinPrice = currentCenterPrice - (newPriceRange * centerPriceRatio);
            double newMaxPrice = currentCenterPrice + (newPriceRange * (1.0 - centerPriceRatio));

            sLog_Probe("viewport.zoom", "delta=" << delta << "->" << clampedDelta
                       << " zoom=" << m_zoomFactor << "->" << newZoom
                       << " mouse=(" << center.x() << "," << center.y() << ")"
                       << " centerRatio=(" << centerTimeRatio << "," << centerPriceRatio << ")"
                       << " time=[" << m_visibleTimeStart_ms << ".." << m_visibleTimeEnd_ms << "]->["
                       << newTimeStart << ".." << newTimeEnd << "]"
                       << " price=[" << m_minPrice << ".." << m_maxPrice << "]->["
                       << newMinPrice << ".." << newMaxPrice << "]");
            if (newTimeEnd <= newTimeStart || newMaxPrice <= newMinPrice) {
                sLog_Warning("Zoom aborted: invalid final bounds time=[" << newTimeStart << ".." << newTimeEnd
                             << "] price=[" << newMinPrice << ".." << newMaxPrice << "]");
                return;
            }
            setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice);
            emit priceInteracted();
        }
        // A zoom-out the clamps stopped does not use up the zoom factor range.
        if (!(clamped && newZoom < m_zoomFactor)) m_zoomFactor = newZoom;
        if (m_autoScrollEnabled) {
            m_autoScrollEnabled = false;
            emit autoScrollEnabledChanged();
        }
    }
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
    m_panVisualOffset += delta;
    m_lastMousePos = position;
    
    emit panVisualOffsetChanged();
}

void GridViewState::handlePanEnd(bool applyViewport) {
    if (!m_isDragging) return;

    m_isDragging = false;
    if (!applyViewport) {
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
    if (zoomMultiplier <= 0.0) {
        return;
    }

    int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    if (currentTimeRange <= 0) {
        currentTimeRange = 1;
    }

    double centerRatio = centerX / viewportWidth;
    centerRatio = std::max(0.0, std::min(1.0, centerRatio));

    const double timeRangeD = static_cast<double>(currentTimeRange) / zoomMultiplier;
    const bool zoomingOut = (processedDelta < 0.0);
    const int64_t newTimeRange = std::min(
        std::max<int64_t>(1, static_cast<int64_t>(zoomingOut ? std::ceil(timeRangeD)
                                                              : std::floor(timeRangeD))),
        timeSpanLimit(m_maxTimeSpanMs));
    const int64_t currentCenterTime =
        m_visibleTimeStart_ms +
        static_cast<int64_t>(static_cast<double>(currentTimeRange) * centerRatio);
    const double newTimeStartD =
        static_cast<double>(currentCenterTime) -
        static_cast<double>(newTimeRange) * centerRatio;
    int64_t newTimeStart = static_cast<int64_t>(std::floor(newTimeStartD));
    int64_t newTimeEnd = newTimeStart + newTimeRange;
    if (newTimeEnd <= newTimeStart) {
        newTimeEnd = newTimeStart + 1;
    }

    setViewport(newTimeStart, newTimeEnd, m_minPrice, m_maxPrice);
    if (!(zoomingOut && newTimeRange <= currentTimeRange))
        m_zoomFactor = std::max(0.1, std::min(MAX_ZOOM_FACTOR, m_zoomFactor * zoomMultiplier));
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
    if (zoomMultiplier <= 0.0) {
        return;
    }

    double currentPriceRange = m_maxPrice - m_minPrice;
    if (currentPriceRange <= 0.0) {
        currentPriceRange = 1.0;
    }

    double centerRatio = 1.0 - (centerY / viewportHeight);
    centerRatio = std::max(0.0, std::min(1.0, centerRatio));

    double newPriceRange = std::max(1e-6, currentPriceRange / zoomMultiplier);
    if (m_maxPriceSpan > 0) newPriceRange = std::min(newPriceRange, m_maxPriceSpan);
    const double currentCenterPrice = m_minPrice + currentPriceRange * centerRatio;
    const double newMinPrice = currentCenterPrice - newPriceRange * centerRatio;
    const double newMaxPrice =
        currentCenterPrice + newPriceRange * (1.0 - centerRatio);
    if (newMaxPrice <= newMinPrice) {
        return;
    }

    setViewport(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, newMinPrice, newMaxPrice);
    emit priceInteracted();
    if (!(processedDelta < 0.0 && newPriceRange <= currentPriceRange))
        m_zoomFactor = std::max(0.1, std::min(MAX_ZOOM_FACTOR, m_zoomFactor * zoomMultiplier));
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::enableAutoScroll(bool enabled) {
    if (m_autoScrollEnabled != enabled) {
        m_autoScrollEnabled = enabled;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::resetZoom() {
    m_zoomFactor = 1.0;
    m_panOffsetTime_ms = 0.0;
    m_panOffsetPrice = 0.0;
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
    if (!m_timeWindowValid) return;
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
    if (!m_timeWindowValid) return;
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
