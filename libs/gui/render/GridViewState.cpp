/*
Sentinel — GridViewState
Role: Implements the mathematical logic for panning and zooming the chart viewport.
Inputs/Outputs: Implements formulas for updating time/price ranges based on mouse deltas.
Threading: All code is designed to be executed on the main GUI thread.
Performance: Calculations are optimized for real-time interactive performance.
Integration: The concrete implementation of the viewport state machine.
Observability: No internal logging; events are logged from the header's declared methods.
Related: GridViewState.hpp.
Assumptions: Zoom logic correctly implements "zoom-to-cursor" functionality.
*/
#include "GridViewState.hpp"
// CoordinateSystem not used directly here
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

    // Map world (time ms, price) → screen pixels
    // World X range: [m_visibleTimeStart_ms, m_visibleTimeEnd_ms]
    // World Y range: [m_minPrice, m_maxPrice] (increase up)
    // Screen: width x height with Y down. We flip Y with a negative scale.

    const double timeRange = static_cast<double>(m_visibleTimeEnd_ms - m_visibleTimeStart_ms);
    const double priceRange = (m_maxPrice - m_minPrice);
    if (timeRange <= 0.0 || priceRange <= 0.0 || m_viewportWidth <= 0.0 || m_viewportHeight <= 0.0) {
        return QMatrix4x4();
    }

    const double sx = m_viewportWidth / timeRange;
    const double sy = -m_viewportHeight / priceRange; // flip Y so higher price is higher on screen

    QMatrix4x4 transform;

    // Scale world to pixels
    transform.scale(sx, sy, 1.0);

    // Translate world origin so timeStart→x=0, priceMax→y=0 after flip
    transform.translate(-static_cast<double>(m_visibleTimeStart_ms), -m_maxPrice, 0.0);

    // Apply visual pan offset in pixel space last (drag feedback)
    if (m_isDragging && !m_panVisualOffset.isNull()) {
        QMatrix4x4 screenSpace;
        screenSpace.translate(m_panVisualOffset.x(), m_panVisualOffset.y());
        transform = screenSpace * transform;
    }

    return transform;
}

void GridViewState::handleZoom(double delta, const QPointF& center) {
    // Delegate to cursor-aware implementation using current viewport size
    handleZoomWithViewport(delta, center, QSizeF(m_viewportWidth, m_viewportHeight));
}

void GridViewState::handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    
    // Apply clamped delta for controlled zoom
    double clampedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, delta));
    double zoomMultiplier = 1.0 + clampedDelta;
    double newZoom = m_zoomFactor * zoomMultiplier;
    
    // Clamp zoom levels (0.1x to 10x)
    newZoom = std::max(0.1, std::min(10.0, newZoom));
    
    if (newZoom != m_zoomFactor) {
        //  ZOOM TO MOUSE POINTER: Calculate center point with proper coordinate conversion
        if (center.x() >= 0 && center.y() >= 0) {
            // Get current viewport ranges
            int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            double currentPriceRange = m_maxPrice - m_minPrice;
            
            // Calculate new ranges based on zoom factor - ensure positive values
            int64_t newTimeRange = static_cast<int64_t>(currentTimeRange * (m_zoomFactor / newZoom));
            double newPriceRange = currentPriceRange * (m_zoomFactor / newZoom);
            
            // Validate ranges to prevent invalid coordinates
            if (newTimeRange <= 0 || newPriceRange <= 0.0) {
                qDebug() << "🚨 ZOOM ABORT: Invalid range calculated - TimeRange:" << newTimeRange << "PriceRange:" << newPriceRange;
                return;
            }
            
            // Convert mouse position to normalized coordinates (0.0 to 1.0)
            double centerTimeRatio = center.x() / viewportSize.width();
            double centerPriceRatio = 1.0 - (center.y() / viewportSize.height());  // Invert Y for price
            
            // Clamp ratios to valid range
            centerTimeRatio = std::max(0.0, std::min(1.0, centerTimeRatio));
            centerPriceRatio = std::max(0.0, std::min(1.0, centerPriceRatio));
            
            if constexpr (kTraceZoomInteractions) {
                qDebug() << " ZOOM:" << "Delta:" << delta << "->" << clampedDelta
                         << "Zoom:" << m_zoomFactor << "->" << newZoom
                         << "Mouse(" << center.x() << "," << center.y() << ")";
            }
            
            // Calculate current center point in data coordinates
            int64_t currentCenterTime = m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio);
            double currentCenterPrice = m_minPrice + (currentPriceRange * centerPriceRatio);
            
            // Update viewport bounds around the center point
            int64_t newTimeStart = currentCenterTime - static_cast<int64_t>(newTimeRange * centerTimeRatio);
            int64_t newTimeEnd = currentCenterTime + static_cast<int64_t>(newTimeRange * (1.0 - centerTimeRatio));
            double newMinPrice = currentCenterPrice - (newPriceRange * centerPriceRatio);
            double newMaxPrice = currentCenterPrice + (newPriceRange * (1.0 - centerPriceRatio));
            
            // Final validation - ensure time range is positive and price range is valid
            if (newTimeEnd <= newTimeStart || newMaxPrice <= newMinPrice) {
                qDebug() << "🚨 ZOOM ABORT: Invalid final bounds - Time[" << newTimeStart << "," << newTimeEnd << "] Price[" << newMinPrice << "," << newMaxPrice << "]";
                return;
            }
            
            // Apply validated bounds via central setter to bump version and signal change
            setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice);
            
            if constexpr (kTraceZoomInteractions) {
                qDebug() << " ZOOM RESULT:"
                         << "OldTime[" << (m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio)) << "]"
                         << "NewTime[" << currentCenterTime << "]"
                         << "TimeRange:" << currentTimeRange << "->" << newTimeRange;
            }
        }
        
        m_zoomFactor = newZoom;
        
        // Disable auto-scroll when user manually zooms
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
    
    // Disable auto-scroll when user starts dragging
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        emit autoScrollEnabledChanged();
    }
}

void GridViewState::handlePanMove(const QPointF& position) {
    if (!m_isDragging) return;
    
    QPointF delta = position - m_lastMousePos;
    m_panVisualOffset += delta;
    m_lastMousePos = position;
    
    emit panVisualOffsetChanged();
}

void GridViewState::handlePanEnd() {
    if (!m_isDragging) return;

    m_isDragging = false;
    
    const double threshold = 1.0; // pixels
    if (m_panVisualOffset.manhattanLength() > threshold && m_viewportWidth > 0 && m_viewportHeight > 0) {
        // Create viewport for coordinate conversion
        // TODO: figure out why we aren't using 'viewport'
        // Convert visual offset to data coordinates using CoordinateSystem
        int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        double priceRange = m_maxPrice - m_minPrice;
        
        double timePixelsToMs = static_cast<double>(timeRange) / m_viewportWidth;
        double pricePixelsToUnits = priceRange / m_viewportHeight;
        
        int64_t timeDelta = static_cast<int64_t>(-m_panVisualOffset.x() * timePixelsToMs);
        double priceDelta = m_panVisualOffset.y() * pricePixelsToUnits;
        
        // Apply to viewport through setViewport
        setViewport(m_visibleTimeStart_ms + timeDelta,
                   m_visibleTimeEnd_ms + timeDelta,
                   m_minPrice + priceDelta,
                   m_maxPrice + priceDelta);
    }
    
    // Do not clear visual offset here; let the renderer clear it
    // after geometry is resynchronized to avoid visual snap-back.
}

void GridViewState::handleZoomWithSensitivity(double rawDelta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    
    // Apply zoom sensitivity and clamp the delta
    double processedDelta = rawDelta * ZOOM_SENSITIVITY;
    processedDelta = std::max(-MAX_ZOOM_DELTA, std::min(MAX_ZOOM_DELTA, processedDelta));
    
    // Use the existing viewport-aware zoom logic
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

// Directional pan methods
void GridViewState::panLeft() {
    if (!m_timeWindowValid) return;
    
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1; // Pan 10% of visible range
    
    setViewport(
        m_visibleTimeStart_ms - panAmount,
        m_visibleTimeEnd_ms - panAmount,
        m_minPrice,
        m_maxPrice
    );
}

void GridViewState::panRight() {
    if (!m_timeWindowValid) return;
    
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1; // Pan 10% of visible range
    
    setViewport(
        m_visibleTimeStart_ms + panAmount,
        m_visibleTimeEnd_ms + panAmount,
        m_minPrice,
        m_maxPrice
    );
}

void GridViewState::panUp() {
    if (!m_timeWindowValid) return;
    
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1; // Pan 10% of visible range
    
    setViewport(
        m_visibleTimeStart_ms,
        m_visibleTimeEnd_ms,
        m_minPrice + panAmount,
        m_maxPrice + panAmount
    );
}

void GridViewState::panDown() {
    if (!m_timeWindowValid) return;
    
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1; // Pan 10% of visible range
    
    setViewport(
        m_visibleTimeStart_ms,
        m_visibleTimeEnd_ms,
        m_minPrice - panAmount,
        m_maxPrice - panAmount
    );
}

double GridViewState::calculateOptimalPriceResolution() const {
    if (!m_timeWindowValid) return 1.0;  // Default fallback
    
    // Calculate price span (this is the key - how much price range is visible)
    double priceSpan = m_maxPrice - m_minPrice;
    
    //  PRICE-SPAN-BASED LOD: Use price range to determine resolution
    // When zoomed out (large price range), use coarser buckets
    if (priceSpan > 500) {               // > $500 range: $25 buckets  
        return 25.0;
    } else if (priceSpan > 100) {        // > $100 range: $5 buckets
        return 5.0;
    } else if (priceSpan > 50) {         // > $50 range: $1 buckets
        return 1.0;
    } else if (priceSpan > 10) {         // > $10 range: $0.50 buckets
        return 0.50;
    } else {                             // <= $10 range: $0.25 buckets
        return 0.25;
    }
}
