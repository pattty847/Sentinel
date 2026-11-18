#pragma once
#include "AxisModel.hpp"
#include "DisplayTime.hpp"
#include <optional>

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
 * Above one day the steps are calendar periods (2 days, 1 and 2 weeks, 1 and 3
 * months, 1 to 100 years) on local midnights of the display zone, chosen as the
 * smallest that keeps ticks at least the label gap apart, held in a band around
 * each switch point. Ticks never exceed the model's label capacity: a step that
 * would make more takes the next larger step (years continue 200, 500, 1000 ...),
 * so the axis is never cut to its leftmost ticks; a step with no tick on screen
 * (or one, for a calendar step) takes a smaller one. A calendar axis names its
 * year at least once, on the first label when no other label has it.
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
    
    // A calendar step (above one day): its nominal length selects and identifies it.
    struct CalendarStep {
        qint64 nominalMs;
        DisplayTime::Calendar unit;
        int count;
    };
    static std::optional<CalendarStep> calendarStep(qint64 stepMs);
    static qint64 largerStep(qint64 stepMs);
    static qint64 smallerStep(qint64 stepMs);
    static qint64 nextTick(qint64 afterMs, qint64 stepMs);
    int countTicks(qint64 stepMs, qint64 timeStart, qint64 timeEnd) const;
    qint64 fitStep(qint64 step, qint64 timeStart, qint64 timeEnd, int targetTicks) const;
    qint64 calculateNiceTimeStep(qint64 rangeMs, int targetTicks) const;
    QString formatTimeLabel(qint64 timestampMs, qint64 stepMs) const;
    static QString formatCalendarLabel(qint64 timestampMs, qint64 previousMs, const CalendarStep& step,
                                       bool withYear, bool* hasYear);
    bool updateEffectiveViewport();
    
    // Predefined nice time steps
    static const std::vector<TimeStep> TIME_STEPS;
    static const std::vector<CalendarStep> CALENDAR_STEPS;

    double m_effectiveStart = 0.0;
    double m_effectiveEnd = 0.0;
    double m_effectiveOffsetPx = 0.0;
    double m_effectiveSpanPx = 0.0;
    bool m_effectiveViewportValid = false;
    qint64 m_lastNiceStepMs = 0;
};
