#pragma once
#include <QPointF>
#include <QMatrix4x4>
#include <QObject>

// Forward declare Viewport outside the class to avoid MOC issues
struct Viewport {
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;
    double width = 800.0;
    double height = 600.0;
};

class CoordinateSystem : public QObject {
    Q_OBJECT

public:
    
    // Core transformation functions
    static QPointF worldToScreen(int64_t timestamp_ms, double price, const Viewport& viewport);
    static QPointF screenToWorld(const QPointF& screenPos, const Viewport& viewport);
    static QMatrix4x4 calculateTransform(const Viewport& viewport);
    
    // Validation and debugging
    static bool validateViewport(const Viewport& viewport);
    static QString viewportDebugString(const Viewport& viewport);
    
private:
    static double normalizeTime(int64_t timestamp_ms, const Viewport& viewport);
    static double normalizePrice(double price, const Viewport& viewport);
    static constexpr double EPSILON = 1e-10;
};

Q_DECLARE_METATYPE(Viewport)