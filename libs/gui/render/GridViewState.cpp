#include "GridViewState.hpp"
#include <QMatrix4x4>
#include <QSizeF>
#include <QDebug>
#include <algorithm>

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
        emit viewportChanged();
    }
}

QMatrix4x4 GridViewState::calculateViewportTransform(const QRectF& itemBounds) const {
    if (!m_timeWindowValid || itemBounds.isEmpty()) {
        return QMatrix4x4();
    }
    
    QMatrix4x4 transform;
    
    // Apply zoom transformation
    if (m_zoomFactor != 1.0) {
        transform.scale(m_zoomFactor, m_zoomFactor);
    }
    
    // Apply pan transformation
    if (m_panOffsetTime_ms != 0.0 || m_panOffsetPrice != 0.0) {
        double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        double priceRange = m_maxPrice - m_minPrice;
        
        if (timeRange > 0 && priceRange > 0) {
            double panX = (m_panOffsetTime_ms / timeRange) * itemBounds.width();
            double panY = (m_panOffsetPrice / priceRange) * itemBounds.height();
            transform.translate(panX, panY);
        }
    }
    
    // Apply visual offset during active dragging
    if (m_isDragging && !m_panVisualOffset.isNull()) {
        transform.translate(m_panVisualOffset.x(), m_panVisualOffset.y());
    }
    
    return transform;
}

void GridViewState::handleZoom(double delta, const QPointF& center) {
    if (!m_timeWindowValid) return;
    
    double zoomMultiplier = 1.0 + (delta > 0 ? 0.1 : -0.1);
    double newZoom = m_zoomFactor * zoomMultiplier;
    
    // Clamp zoom levels (0.1x to 10x)
    newZoom = std::max(0.1, std::min(10.0, newZoom));
    
    if (newZoom != m_zoomFactor) {
        // 🎯 ZOOM TO MOUSE POINTER: Calculate center point in data coordinates
        if (center.x() >= 0 && center.y() >= 0) {  // Valid center point provided
            // Get current viewport ranges
            int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            double currentPriceRange = m_maxPrice - m_minPrice;
            
            // Calculate new ranges based on zoom factor
            int64_t newTimeRange = static_cast<int64_t>(currentTimeRange * (m_zoomFactor / newZoom));
            double newPriceRange = currentPriceRange * (m_zoomFactor / newZoom);
            
            // Calculate center point ratios (0.0 to 1.0) from pixel coordinates
            // Note: We need the viewport size to convert pixels to ratios
            // For now, we'll use a simple approach assuming the center is already normalized
            double centerTimeRatio = 0.5;  // Default to center for now
            double centerPriceRatio = 0.5;  // Default to center for now
            
            // Calculate current center in data coordinates
            int64_t currentCenterTime = m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio);
            double currentCenterPrice = m_minPrice + (currentPriceRange * centerPriceRatio);
            
            // Update viewport bounds around the center point
            m_visibleTimeStart_ms = currentCenterTime - static_cast<int64_t>(newTimeRange * centerTimeRatio);
            m_visibleTimeEnd_ms = currentCenterTime + static_cast<int64_t>(newTimeRange * (1.0 - centerTimeRatio));
            m_minPrice = currentCenterPrice - (newPriceRange * centerPriceRatio);
            m_maxPrice = currentCenterPrice + (newPriceRange * (1.0 - centerPriceRatio));
        }
        
        m_zoomFactor = newZoom;
        
        // Disable auto-scroll when user manually zooms
        if (m_autoScrollEnabled) {
            m_autoScrollEnabled = false;
            emit autoScrollEnabledChanged();
        }
        
        emit viewportChanged();
    }
}

void GridViewState::handleZoomWithViewport(double delta, const QPointF& center, const QSizeF& viewportSize) {
    if (!m_timeWindowValid || viewportSize.isEmpty()) return;
    
    double zoomMultiplier = 1.0 + (delta > 0 ? 0.1 : -0.1);
    double newZoom = m_zoomFactor * zoomMultiplier;
    
    // Clamp zoom levels (0.1x to 10x)
    newZoom = std::max(0.1, std::min(10.0, newZoom));
    
    if (newZoom != m_zoomFactor) {
        // 🎯 ZOOM TO MOUSE POINTER: Calculate center point with proper coordinate conversion
        if (center.x() >= 0 && center.y() >= 0) {
            // Get current viewport ranges
            int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            double currentPriceRange = m_maxPrice - m_minPrice;
            
            // Calculate new ranges based on zoom factor
            int64_t newTimeRange = static_cast<int64_t>(currentTimeRange * (m_zoomFactor / newZoom));
            double newPriceRange = currentPriceRange * (m_zoomFactor / newZoom);
            
            // Convert mouse position to normalized coordinates (0.0 to 1.0)
            double centerTimeRatio = center.x() / viewportSize.width();
            double centerPriceRatio = 1.0 - (center.y() / viewportSize.height());  // Invert Y for price
            
            // Clamp ratios to valid range
            centerTimeRatio = std::max(0.0, std::min(1.0, centerTimeRatio));
            centerPriceRatio = std::max(0.0, std::min(1.0, centerPriceRatio));
            
            // 🐛 DEBUG: Log the zoom calculation
            qDebug() << "🔍 ZOOM DEBUG:"
                     << "Mouse(" << center.x() << "," << center.y() << ")"
                     << "Viewport(" << viewportSize.width() << "x" << viewportSize.height() << ")"
                     << "Ratios(" << centerTimeRatio << "," << centerPriceRatio << ")"
                     << "OldZoom:" << m_zoomFactor << "NewZoom:" << newZoom;
            
            // Calculate current center point in data coordinates
            int64_t currentCenterTime = m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio);
            double currentCenterPrice = m_minPrice + (currentPriceRange * centerPriceRatio);
            
            // Update viewport bounds around the center point
            m_visibleTimeStart_ms = currentCenterTime - static_cast<int64_t>(newTimeRange * centerTimeRatio);
            m_visibleTimeEnd_ms = currentCenterTime + static_cast<int64_t>(newTimeRange * (1.0 - centerTimeRatio));
            m_minPrice = currentCenterPrice - (newPriceRange * centerPriceRatio);
            m_maxPrice = currentCenterPrice + (newPriceRange * (1.0 - centerPriceRatio));
            
            qDebug() << "🔍 ZOOM RESULT:"
                     << "OldTime[" << (m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio)) << "]"
                     << "NewTime[" << currentCenterTime << "]"
                     << "TimeRange:" << currentTimeRange << "->" << newTimeRange;
        }
        
        m_zoomFactor = newZoom;
        
        // Disable auto-scroll when user manually zooms
        if (m_autoScrollEnabled) {
            m_autoScrollEnabled = false;
            emit autoScrollEnabledChanged();
        }
        
        emit viewportChanged();
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
    
    // Reset drag state - parent handles coordinate conversion
    m_isDragging = false;
    m_panVisualOffset = QPointF(0, 0);
    
    emit panVisualOffsetChanged();
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