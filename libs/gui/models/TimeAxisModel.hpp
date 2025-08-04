#pragma once
#include "AxisModel.hpp"
#include <QDateTime>

/**
 * TimeAxisModel - Calculates nice time tick marks
 * 
 * Specializes AxisModel for time data. Calculates "nice" time steps
 * like 1s, 5s, 15s, 30s, 1min, 5min, etc. and formats labels with
 * appropriate precision (HH:MM:SS.ms, HH:MM:SS, HH:MM).
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
    
    // Predefined nice time steps
    static const std::vector<TimeStep> TIME_STEPS;
};