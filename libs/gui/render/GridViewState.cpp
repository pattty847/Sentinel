// GridViewState: viewport pan/zoom math; main GUI thread only.
#include "GridViewState.hpp"
#include <QMatrix4x4>
#include <QSizeF>
#include <QDebug>
#include <algorithm>

namespace {
constexpr bool kTraceZoomInteractions = false;
}

GridViewState::GridViewState(QObject* parent) 
    : QObject(parent) {
    m_interactionTimer.start();
}

void GridViewState::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) {
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
        emit viewportChanged();
    }
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

    static const bool kZoomDebug = qEnvironmentVariableIsSet("SENTINEL_ZOOM_DEBUG");
    double clampedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, delta));
    double zoomMultiplier = 1.0 + clampedDelta;
    double newZoom = m_zoomFactor * zoomMultiplier;
    newZoom = std::max(0.1, std::min(MAX_ZOOM_FACTOR, newZoom));
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
            const int64_t newTimeRange = std::max<int64_t>(
                1,
                static_cast<int64_t>(zoomingOut ? std::ceil(timeRangeD) : std::floor(timeRangeD))
            );
            const double newPriceRange = std::max(1e-6, currentPriceRange * (m_zoomFactor / newZoom));
            if (newTimeRange <= 0 || newPriceRange <= 0.0) {
                qDebug() << "🚨 ZOOM ABORT: Invalid range calculated - TimeRange:" << newTimeRange << "PriceRange:" << newPriceRange;
                return;
            }
            double centerTimeRatio = center.x() / viewportSize.width();
            double centerPriceRatio = 1.0 - (center.y() / viewportSize.height());
            centerTimeRatio = std::max(0.0, std::min(1.0, centerTimeRatio));
            centerPriceRatio = std::max(0.0, std::min(1.0, centerPriceRatio));
            
            if constexpr (kTraceZoomInteractions) {
                qDebug() << " ZOOM:" << "Delta:" << delta << "->" << clampedDelta
                         << "Zoom:" << m_zoomFactor << "->" << newZoom
                         << "Mouse(" << center.x() << "," << center.y() << ")";
            }
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
            
            if (kZoomDebug) {
                qDebug() << "ZOOM DEBUG:"
                         << "curTimeRange" << currentTimeRange
                         << "curPriceRange" << currentPriceRange
                         << "newTimeRange" << newTimeRange
                         << "newPriceRange" << newPriceRange
                         << "centerRatios" << centerTimeRatio << centerPriceRatio
                         << "bounds" << m_visibleTimeStart_ms << m_visibleTimeEnd_ms
                         << m_minPrice << m_maxPrice;
            }
            if (newTimeEnd <= newTimeStart || newMaxPrice <= newMinPrice) {
                qDebug() << "🚨 ZOOM ABORT: Invalid final bounds - Time[" << newTimeStart << "," << newTimeEnd << "] Price[" << newMinPrice << "," << newMaxPrice << "]";
                return;
            }
            setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice);
            
            if constexpr (kTraceZoomInteractions) {
                qDebug() << " ZOOM RESULT:"
                         << "OldTime[" << (m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio)) << "]"
                         << "NewTime[" << currentCenterTime << "]"
                         << "TimeRange:" << currentTimeRange << "->" << newTimeRange;
            }
        }
        m_zoomFactor = newZoom;
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
    }
}

void GridViewState::handleZoomWithSensitivity(double rawDelta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    double processedDelta = rawDelta * ZOOM_SENSITIVITY;
    processedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, processedDelta));
    handleZoomWithViewport(processedDelta, center, viewportSize);
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
