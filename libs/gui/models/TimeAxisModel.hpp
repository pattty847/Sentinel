#pragma once
#include "AxisModel.hpp"

/**
 * TimeAxisModel - Calculates nice time tick marks
 * 
 * Specializes AxisModel for time data. Calculates "nice" time steps
 * like 1s, 5s, 15s, 30s, 1min, 5min, etc. and formats labels with
 * appropriate precision.
 *
 * Labels and the grid's boundaries follow the chart's display time zone
 * (DisplayTime): in Local mode a 1h tick lands on a local hour and a 1d tick on a
 * local midnight. A zone change re-labels at once.
 * 
 * Uses TradingView-style adaptive formatting:
 * - Minimal labels (just numbers when context is clear)
 * - Context labels at boundaries (day/month/year changes)
 */
class TimeAxisModel : public AxisModel {
    Q_OBJECT

public:
    explicit TimeAxisModel(QObject* parent = nullptr);

    // AxisModel interface
    void recalculateTicks() override;
    
protected:
    void calculateTicks() override;
    QString formatLabel(double value) const override;
    double getViewportStart() const override;
    double getViewportEnd() const override;
    double valueToScreenPosition(double value) const override;
    
private:
    struct TimeStep {
        qint64 milliseconds;
        QString description;
        
        TimeStep(qint64 ms, const QString& desc) : milliseconds(ms), description(desc) {}
    };
    
    qint64 calculateNiceTimeStep(qint64 rangeMs, int targetTicks) const;
    QString formatTimeLabel(qint64 timestampMs, qint64 stepMs) const;
    bool updateEffectiveViewport();
    
    // Predefined nice time steps
    static const std::vector<TimeStep> TIME_STEPS;

    double m_effectiveStart = 0.0;
    double m_effectiveEnd = 0.0;
    double m_effectiveOffsetPx = 0.0;
    double m_effectiveSpanPx = 0.0;
    bool m_effectiveViewportValid = false;
    qint64 m_lastNiceStepMs = 0;
};
