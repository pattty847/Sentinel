#include "chartcoordinator.h"
#include <cmath>

ChartCoordinator::ChartCoordinator(QObject* parent) : QObject(parent) {}

qint64 ChartCoordinator::quantizeTimestamp(qint64 timestamp_ms) const {
    if (m_bucketSize_ms <= 0) return timestamp_ms;
    return floor(static_cast<double>(timestamp_ms) / m_bucketSize_ms) * m_bucketSize_ms;
}

int ChartCoordinator::timestampToFrameIndex(qint64 timestamp_ms) const {
    if (m_bucketSize_ms <= 0) return 0;
    return floor(static_cast<double>(timestamp_ms) / m_bucketSize_ms);
}

qint64 ChartCoordinator::frameIndexToTimestamp(int frameIndex) const {
    return frameIndex * m_bucketSize_ms;
}

QPointF ChartCoordinator::worldToScreen(qint64 timestamp_ms, double price) const {
    if (m_viewRect.width() <= 0 || m_viewRect.height() <= 0 || m_endTime_ms <= m_startTime_ms || m_maxPrice <= m_minPrice) {
        return QPointF();
    }

    double timeRange = m_endTime_ms - m_startTime_ms;
    double priceRange = m_maxPrice - m_minPrice;

    double x = (timestamp_ms - m_startTime_ms) / timeRange * m_viewRect.width();
    double y = (1.0 - (price - m_minPrice) / priceRange) * m_viewRect.height();

    return QPointF(x, y);
}

QPointF ChartCoordinator::screenToWorld(const QPointF& screenPos) const {
    if (m_viewRect.width() <= 0 || m_viewRect.height() <= 0 || m_endTime_ms <= m_startTime_ms || m_maxPrice <= m_minPrice) {
        return QPointF();
    }

    double timeRange = m_endTime_ms - m_startTime_ms;
    double priceRange = m_maxPrice - m_minPrice;

    qint64 timestamp = m_startTime_ms + (screenPos.x() / m_viewRect.width()) * timeRange;
    double price = m_minPrice + (1.0 - (screenPos.y() / m_viewRect.height())) * priceRange;

    return QPointF(timestamp, price);
}

void ChartCoordinator::setViewRect(const QRectF& viewRect) {
    if (m_viewRect != viewRect) {
        m_viewRect = viewRect;
        emit viewChanged();
    }
}

void ChartCoordinator::setTimeRange(qint64 startTime_ms, qint64 endTime_ms) {
    if (m_startTime_ms != startTime_ms || m_endTime_ms != endTime_ms) {
        m_startTime_ms = startTime_ms;
        m_endTime_ms = endTime_ms;
        emit viewChanged();
    }
}

void ChartCoordinator::setPriceRange(double minPrice, double maxPrice) {
    if (m_minPrice != minPrice || m_maxPrice != maxPrice) {
        m_minPrice = minPrice;
        m_maxPrice = maxPrice;
        emit viewChanged();
    }
}
