#pragma once

#include <QObject>
#include <QRectF>

class ChartCoordinator : public QObject {
    Q_OBJECT
public:
    explicit ChartCoordinator(QObject* parent = nullptr);

    // --- Time Quantization ---
    qint64 quantizeTimestamp(qint64 timestamp_ms) const;
    int timestampToFrameIndex(qint64 timestamp_ms) const;
    qint64 frameIndexToTimestamp(int frameIndex) const;

    // --- Coordinate Transformation ---
    QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    QPointF screenToWorld(const QPointF& screenPos) const;

public slots:
    // --- Viewport Management ---
    void setViewRect(const QRectF& viewRect);
    void setTimeRange(qint64 startTime_ms, qint64 endTime_ms);
    void setPriceRange(double minPrice, double maxPrice);

signals:
    void viewChanged();

private:
    QRectF m_viewRect;
    qint64 m_startTime_ms = 0;
    qint64 m_endTime_ms = 0;
    double m_minPrice = 0;
    double m_maxPrice = 0;
    qint64 m_bucketSize_ms = 1000; // Default to 1 second
};
