#include "TimeAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include "DisplayTime.hpp"
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>
#include <optional>

const std::vector<TimeAxisModel::TimeStep> TimeAxisModel::TIME_STEPS = {
    {25, "25ms"}, {50, "50ms"}, {100, "100ms"}, {250, "250ms"}, {500, "500ms"},
    {1000, "1s"}, {2000, "2s"}, {5000, "5s"}, {10000, "10s"}, {15000, "15s"},
    {30000, "30s"}, {60000, "1min"}, {120000, "2min"}, {300000, "5min"},
    {600000, "10min"}, {900000, "15min"}, {1800000, "30min"}, {3600000, "1h"},
    {7200000, "2h"}, {14400000, "4h"}, {21600000, "6h"}, {43200000, "12h"},
    {86400000, "1d"}
};

namespace {
constexpr qint64 kDayMs = 86400000;
constexpr qint64 kYearMs = 31556952000; // 365.2425 days
constexpr qint64 kMaxYears = 10'000'000;  // under DisplayTime's 2^60 ms boundary limit
} // namespace
using Calendar = DisplayTime::Calendar;
const std::vector<TimeAxisModel::CalendarStep> TimeAxisModel::CALENDAR_STEPS = {
    {2 * kDayMs, Calendar::Days, 2},       {7 * kDayMs, Calendar::Weeks, 1},
    {14 * kDayMs, Calendar::Weeks, 2},     {kYearMs / 12, Calendar::Months, 1},
    {kYearMs / 4, Calendar::Months, 3},    {kYearMs, Calendar::Months, 12},
    {2 * kYearMs, Calendar::Months, 24},   {5 * kYearMs, Calendar::Months, 60},
    {10 * kYearMs, Calendar::Months, 120}, {20 * kYearMs, Calendar::Months, 240},
    {50 * kYearMs, Calendar::Months, 600}, {100 * kYearMs, Calendar::Months, 1200},
};

// Beyond the table, whole numbers of years in a 1-2-5 series (200, 500, 1000 ...
// years) up to kMaxYears: January 1sts of their multiples, so they stay anchored.
std::optional<TimeAxisModel::CalendarStep> TimeAxisModel::calendarStep(qint64 stepMs) {
    for (const auto& step : CALENDAR_STEPS)
        if (step.nominalMs == stepMs) return step;
    if (stepMs > CALENDAR_STEPS.back().nominalMs && stepMs % kYearMs == 0 && stepMs / kYearMs <= kMaxYears)
        return CalendarStep{stepMs, Calendar::Months, int(stepMs / kYearMs * 12)};
    return std::nullopt;
}

qint64 TimeAxisModel::largerStep(qint64 stepMs) {
    for (const auto& step : TIME_STEPS)
        if (step.milliseconds > stepMs) return step.milliseconds;
    for (const auto& step : CALENDAR_STEPS)
        if (step.nominalMs > stepMs) return step.nominalMs;
    const qint64 years = stepMs / kYearMs;
    qint64 decade = 1;
    while (decade * 10 <= years) decade *= 10;
    const qint64 mantissa = years / decade;
    const qint64 next = (mantissa < 2 ? 2 : mantissa < 5 ? 5 : 10) * decade;
    return next <= kMaxYears ? next * kYearMs : stepMs; // stepMs: none larger
}

qint64 TimeAxisModel::smallerStep(qint64 stepMs) {
    if (stepMs > CALENDAR_STEPS.back().nominalMs) {
        const qint64 years = stepMs / kYearMs;
        qint64 decade = 1;
        while (decade * 10 <= years) decade *= 10;
        const qint64 mantissa = years / decade;
        const qint64 previous = mantissa >= 10 ? 5 * decade : mantissa >= 5 ? 2 * decade : mantissa >= 2 ? decade : decade / 2;
        if (previous * kYearMs > CALENDAR_STEPS.back().nominalMs) return previous * kYearMs;
    }
    qint64 best = stepMs;
    for (const auto& step : TIME_STEPS)
        if (step.milliseconds < stepMs) best = step.milliseconds;
    for (const auto& step : CALENDAR_STEPS)
        if (step.nominalMs < stepMs) best = step.nominalMs;
    return best;
}

qint64 TimeAxisModel::nextTick(qint64 afterMs, qint64 stepMs) {
    const DisplayTime& zone = DisplayTime::instance();
    if (const auto step = calendarStep(stepMs)) return zone.nextCalendarBoundary(afterMs, step->unit, step->count);
    return zone.nextBoundary(afterMs, stepMs);
}

// The on-screen ticks of `stepMs`, counted up to the label capacity plus one.
int TimeAxisModel::countTicks(qint64 stepMs, qint64 timeStart, qint64 timeEnd) const {
    int count = 0;
    for (qint64 t = nextTick(timeStart - stepMs, stepMs); t <= timeEnd + stepMs && count <= m_labelCapacity;
         t = nextTick(t, stepMs)) {
        const double x = valueToScreenPosition(static_cast<double>(t));
        if (x >= 0 && x <= getViewportWidth()) ++count;
    }
    return count;
}

// The step to draw: never more ticks than the axis holds (a larger step, not the
// leftmost ticks), and at least one tick on screen (two for calendar steps when the
// target allows): a calendar step can miss a narrow window entirely, so a smaller
// step that fits takes over.
qint64 TimeAxisModel::fitStep(qint64 step, qint64 timeStart, qint64 timeEnd, int targetTicks) const {
    int count = countTicks(step, timeStart, timeEnd);
    for (int i = 0; i < 64 && count > m_labelCapacity; ++i) {
        const qint64 larger = largerStep(step);
        if (larger == step) break;
        step = larger;
        count = countTicks(step, timeStart, timeEnd);
    }
    const int wanted = calendarStep(step) ? std::clamp(targetTicks, 1, 2) : 1;
    for (int i = 0; i < 64 && count < wanted; ++i) {
        const qint64 smaller = smallerStep(step);
        if (smaller == step) break;
        const int smallerCount = countTicks(smaller, timeStart, timeEnd);
        if (smallerCount > m_labelCapacity) break;
        step = smaller;
        count = smallerCount;
    }
    return step;
}

TimeAxisModel::TimeAxisModel(QObject* parent)
    : AxisModel(parent) {
    connect(&DisplayTime::instance(), &DisplayTime::zoneChanged, this, &TimeAxisModel::recalculateTicks);
}

void TimeAxisModel::recalculateTicks() {
    updateTicksAndNotify();
}

bool TimeAxisModel::updateEffectiveViewport() {
    m_effectiveViewportValid = false;
    if (!isViewportValid()) {
        return false;
    }

    // The drawn window (raster camera, drag included as whole device pixels); the
    // committed viewport without a renderer.
    refreshRaster();
    const double viewStart = m_raster.valid ? m_raster.drawnStartMs : getViewportStart();
    const double viewEnd = m_raster.valid ? m_raster.drawnEndMs : getViewportEnd();
    const double viewSpan = viewEnd - viewStart;
    const double viewWidth = m_raster.valid ? m_raster.widthDev / m_raster.dpr : getViewportWidth();
    if (viewSpan <= 0.0 || viewWidth <= 0.0) {
        return false;
    }

    m_effectiveStart = viewStart;
    m_effectiveEnd = viewEnd;
    m_effectiveOffsetPx = 0.0;
    m_effectiveSpanPx = viewWidth;
    m_effectiveViewportValid = true;
    // Axis labels track the full viewport, not just the currently filled
    // heatmap overlap, so future timestamps remain visible as the view leads data.
    return m_effectiveViewportValid;
}

void TimeAxisModel::calculateTicks() {
    clearTicks();
    
    if (!isViewportValid()) return;
    if (!updateEffectiveViewport()) return;
    
    qint64 timeStart = static_cast<qint64>(m_effectiveStart);
    qint64 timeEnd = static_cast<qint64>(m_effectiveEnd);
    qint64 timeRange = timeEnd - timeStart;
    
    if (timeRange <= 0) return;
    
    constexpr double kMinLabelGapPx = 80.0;
    const double viewportPx = std::max(1.0, m_effectiveSpanPx);
    int maxLabelCount = static_cast<int>(std::floor(viewportPx / kMinLabelGapPx));
    maxLabelCount = std::max(2, maxLabelCount);
    const int targetTicks = std::max(1, maxLabelCount - 1);

    qint64 step = calculateNiceTimeStep(timeRange, targetTicks);
    if (step <= 0) return;

    const qint64 rawStep = timeRange / targetTicks;
    const auto lastCalendar = m_lastNiceStepMs > 0 ? calendarStep(m_lastNiceStepMs) : std::nullopt;
    if (lastCalendar && rawStep > TIME_STEPS.back().milliseconds) {
        // Calendar steps are 2x or more apart: hold the current one in a band around
        // each switch point (up to 1.2x above it, down to 0.83x the next smaller).
        if (double(rawStep) <= 1.2 * double(m_lastNiceStepMs) && double(rawStep) >= 0.83 * double(smallerStep(m_lastNiceStepMs)))
            step = m_lastNiceStepMs;
    } else if (m_lastNiceStepMs > 0) {
        const double ratio = static_cast<double>(step) / static_cast<double>(m_lastNiceStepMs);
        if (ratio <= 1.2 && ratio >= 0.83) {
            step = m_lastNiceStepMs;
        }
    }
    step = fitStep(step, timeStart, timeEnd, targetTicks);
    m_lastNiceStepMs = step;
    // Boundaries of the display zone's wall clock (UTC: multiples of the step), or
    // its calendar periods above one day.
    const auto calendar = calendarStep(step);
    qint64 previous = std::numeric_limits<qint64>::min(), first = previous;
    bool yearShown = false;
    int generated = 0;
    for (qint64 timestamp = nextTick(timeStart - step, step); timestamp <= timeEnd + step && generated <= m_labelCapacity;
         timestamp = nextTick(timestamp, step)) {
        double screenX = valueToScreenPosition(static_cast<double>(timestamp));
        if (screenX >= 0 && screenX <= getViewportWidth()) {
            bool hasYear = false;
            QString label = calendar ? formatCalendarLabel(timestamp, previous, *calendar, false, &hasYear)
                                     : formatTimeLabel(timestamp, step);
            addTick(static_cast<double>(timestamp), screenX, label, true);
            yearShown = yearShown || hasYear;
            if (first == std::numeric_limits<qint64>::min()) first = timestamp;
            previous = timestamp;
            ++generated;
        }
    }
    // A calendar axis always names its year once: on the first label when no other has it.
    if (calendar && !yearShown && first != std::numeric_limits<qint64>::min() && !m_ticks.empty())
        m_ticks.front().label = formatCalendarLabel(first, std::numeric_limits<qint64>::min(), *calendar, true, nullptr);
    
    //qDebug() << "TimeAxisModel: Generated" << m_ticks.size() 
    //         << "time ticks for range" << timeRange << "ms, step=" << step << "ms";
}

QString TimeAxisModel::formatLabel(double value) const {
    qint64 timestampMs = static_cast<qint64>(value);
    qint64 rangeMs = m_effectiveViewportValid
        ? static_cast<qint64>(m_effectiveEnd - m_effectiveStart)
        : static_cast<qint64>(getViewportEnd() - getViewportStart());
    
    return formatTimeLabel(timestampMs, rangeMs);
}

double TimeAxisModel::getViewportStart() const {
    return m_viewState ? static_cast<double>(m_viewState->getVisibleTimeStart()) : 0.0;
}

double TimeAxisModel::getViewportEnd() const {
    return m_viewState ? static_cast<double>(m_viewState->getVisibleTimeEnd()) : 60000.0;
}

double TimeAxisModel::valueToScreenPosition(double value) const {
    if (!isViewportValid()) return 0.0;
    if (m_effectiveViewportValid && m_raster.valid) return m_raster.xDev(value) / m_raster.dpr;

    if (m_effectiveViewportValid && m_effectiveEnd > m_effectiveStart) {
        double normalized = (value - m_effectiveStart) / (m_effectiveEnd - m_effectiveStart);
        return m_effectiveOffsetPx + normalized * m_effectiveSpanPx;
    }

    double timeStart = getViewportStart();
    double timeEnd = getViewportEnd();
    
    if (timeEnd <= timeStart) return 0.0;
    double normalized = (value - timeStart) / (timeEnd - timeStart);
    
    return normalized * getViewportWidth();
}

qint64 TimeAxisModel::calculateNiceTimeStep(qint64 rangeMs, int targetTicks) const {
    if (rangeMs <= 0 || targetTicks <= 0) return 1000; // Default 1 second
    
    qint64 rawStep = rangeMs / targetTicks;
    // Above a day: the smallest calendar step at least the raw step, so labels keep
    // their gap (the steps are 2x apart or more).
    if (rawStep > TIME_STEPS.back().milliseconds) {
        qint64 step = CALENDAR_STEPS.front().nominalMs;
        for (int i = 0; i < 64 && step < rawStep; ++i) {
            const qint64 larger = largerStep(step);
            if (larger == step) break;
            step = larger;
        }
        return step;
    }
    auto it = std::lower_bound(TIME_STEPS.begin(), TIME_STEPS.end(), rawStep,
        [](const TimeStep& step, qint64 value) {
            return step.milliseconds < value;
        });
    
    if (it == TIME_STEPS.end()) {
        return TIME_STEPS.back().milliseconds;
    } else if (it == TIME_STEPS.begin()) {
        return TIME_STEPS.front().milliseconds;
    } else {
        auto prev = it - 1;
        qint64 currDiff = std::abs(it->milliseconds - rawStep);
        qint64 prevDiff = std::abs(prev->milliseconds - rawStep);
        
        return (currDiff < prevDiff) ? it->milliseconds : prev->milliseconds;
    }
}

QString TimeAxisModel::formatTimeLabel(qint64 timestampMs, qint64 stepMs) const {
    const QDateTime dateTime = DisplayTime::instance().at(timestampMs);
    if (stepMs < 100) {
        int ms = dateTime.time().msec();
        int second = dateTime.time().second();
        if (second == 0 && ms == 0) {
            return dateTime.toString(":ss");
        }
        return QString(".%1").arg(ms, 3, 10, QChar('0'));
    } else if (stepMs >= 86400000) {
        int day = dateTime.date().day();
        if (day == 1) return dateTime.toString("MMM");
        return QString::number(day);
    } else if (stepMs >= 3600000) {
        int hour = dateTime.time().hour();
        if (hour == 0) return dateTime.toString("d MMM");
        return dateTime.toString("HH:mm");
    } else if (stepMs >= 60000) {
        int minute = dateTime.time().minute();
        if (minute == 0) return dateTime.toString("HH:mm");
        return dateTime.toString(":mm");
    } else if (stepMs >= 1000) {
        int second = dateTime.time().second();
        if (second == 0) return dateTime.toString("HH:mm");
        return dateTime.toString(":ss");
    } else {
        int ms = dateTime.time().msec();
        if (ms == 0) return dateTime.toString(":ss");
        return QString(".%1").arg(ms, 3, 10, QChar('0'));
    }
}

// Calendar steps keep the date-label style: "d MMM" for days and weeks (with the
// year when it changes from the previous label), "MMM" for months with the year
// on January, and the year alone for year steps. `withYear` adds the year
// ("5 Jan 2026", "Feb 2026"); `hasYear` reports whether the label names it.
QString TimeAxisModel::formatCalendarLabel(qint64 timestampMs, qint64 previousMs, const CalendarStep& step,
                                           bool withYear, bool* hasYear) {
    const DisplayTime& zone = DisplayTime::instance();
    const QDateTime at = zone.at(timestampMs);
    QString pattern;
    if (step.unit == Calendar::Months) {
        const bool yearOnly = step.count >= 12 || at.date().month() == 1;
        pattern = yearOnly ? QStringLiteral("yyyy") : withYear ? QStringLiteral("MMM yyyy") : QStringLiteral("MMM");
    } else {
        const bool newYear = previousMs != std::numeric_limits<qint64>::min() &&
                             zone.at(previousMs).date().year() != at.date().year();
        pattern = newYear || withYear ? QStringLiteral("d MMM yyyy") : QStringLiteral("d MMM");
    }
    if (hasYear) *hasYear = pattern.contains(QLatin1String("yyyy"));
    return at.toString(pattern);
}
