#include "CoordinateSystem.h"
#include <QDebug>
#include <cmath>

QPointF CoordinateSystem::worldToScreen(int64_t timestamp_ms, double price, const Viewport& viewport) {
    if (!validateViewport(viewport)) {
        qWarning() << "Invalid viewport:" << viewportDebugString(viewport);
        return QPointF(0, 0);
    }
    
    double normalizedTime = normalizeTime(timestamp_ms, viewport);
    double normalizedPrice = normalizePrice(price, viewport);
    
    // Clamp to avoid rendering outside viewport
    normalizedTime = qBound(0.0, normalizedTime, 1.0);
    normalizedPrice = qBound(0.0, normalizedPrice, 1.0);
    
    double x = normalizedTime * viewport.width;
    double y = (1.0 - normalizedPrice) * viewport.height;  // Flip Y for screen coordinates
    
    return QPointF(x, y);
}

QPointF CoordinateSystem::screenToWorld(const QPointF& screenPos, const Viewport& viewport) {
    if (!validateViewport(viewport)) {
        return QPointF(0, 0);
    }
    
    double normalizedTime = screenPos.x() / viewport.width;
    double normalizedPrice = 1.0 - (screenPos.y() / viewport.height);
    
    int64_t timestamp_ms = viewport.timeStart_ms + 
        static_cast<int64_t>(normalizedTime * (viewport.timeEnd_ms - viewport.timeStart_ms));
    double price = viewport.priceMin + normalizedPrice * (viewport.priceMax - viewport.priceMin);
    
    return QPointF(timestamp_ms, price);
}

QMatrix4x4 CoordinateSystem::calculateTransform(const Viewport& viewport) {
    QMatrix4x4 transform;
    
    // Scale from normalized [0,1] to screen coordinates
    transform.scale(viewport.width, viewport.height, 1.0);
    
    // Flip Y axis for screen coordinates
    transform.scale(1.0, -1.0, 1.0);
    transform.translate(0.0, -1.0, 0.0);
    
    return transform;
}

bool CoordinateSystem::validateViewport(const Viewport& viewport) {
    return viewport.timeEnd_ms > viewport.timeStart_ms &&
           viewport.priceMax > viewport.priceMin &&
           viewport.width > EPSILON &&
           viewport.height > EPSILON;
}

QString CoordinateSystem::viewportDebugString(const Viewport& viewport) {
    return QString("Viewport{time: %1-%2ms, price: %3-%4, size: %5x%6}")
        .arg(viewport.timeStart_ms)
        .arg(viewport.timeEnd_ms)
        .arg(viewport.priceMin)
        .arg(viewport.priceMax)
        .arg(viewport.width)
        .arg(viewport.height);
}

double CoordinateSystem::normalizeTime(int64_t timestamp_ms, const Viewport& viewport) {
    int64_t timeRange = viewport.timeEnd_ms - viewport.timeStart_ms;
    if (timeRange <= 0) return 0.0;
    
    return static_cast<double>(timestamp_ms - viewport.timeStart_ms) / timeRange;
}

double CoordinateSystem::normalizePrice(double price, const Viewport& viewport) {
    double priceRange = viewport.priceMax - viewport.priceMin;
    if (priceRange <= EPSILON) return 0.0;
    
    return (price - viewport.priceMin) / priceRange;
}