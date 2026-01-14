#pragma once
#include "AxisModel.hpp"
#include <QDateTime>
#include <QTimeZone>

/**
 * TimeAxisModel - Calculates nice time tick marks
 * 
 * Specializes AxisModel for time data. Calculates "nice" time steps
 * like 1s, 5s, 15s, 30s, 1min, 5min, etc. and formats labels with
 * appropriate precision. Supports timezone configuration.
 * 
 * Uses TradingView-style adaptive formatting:
 * - Minimal labels (just numbers when context is clear)
 * - Context labels at boundaries (day/month/year changes)
 */
class TimeAxisModel : public AxisModel {
    Q_OBJECT
    Q_PROPERTY(QString timezone READ timezone WRITE setTimezone NOTIFY timezoneChanged)
    
public:
    explicit TimeAxisModel(QObject* parent = nullptr);
    
    // Timezone support
    QString timezone() const;
    void setTimezone(const QString& tzId);
    
signals:
    void timezoneChanged();
    
public:
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
    
    // Timezone for display formatting (default UTC)
    QTimeZone m_timezone = QTimeZone::utc();
};