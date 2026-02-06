#include "TimeAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include "../UnifiedGridRenderer.h"
#include <QDebug>
#include <cmath>
#include <algorithm>

const std::vector<TimeAxisModel::TimeStep> TimeAxisModel::TIME_STEPS = {
    {25, "25ms"}, {50, "50ms"}, {100, "100ms"}, {250, "250ms"}, {500, "500ms"},
    {1000, "1s"}, {2000, "2s"}, {5000, "5s"}, {10000, "10s"}, {15000, "15s"},
    {30000, "30s"}, {60000, "1min"}, {120000, "2min"}, {300000, "5min"},
    {600000, "10min"}, {900000, "15min"}, {1800000, "30min"}, {3600000, "1h"},
    {7200000, "2h"}, {14400000, "4h"}, {21600000, "6h"}, {43200000, "12h"},
    {86400000, "1d"}
};

TimeAxisModel::TimeAxisModel(QObject* parent)
    : AxisModel(parent)
    , m_timezone(QTimeZone::utc()) {
}

QString TimeAxisModel::timezone() const {
    return QString::fromLatin1(m_timezone.id());
}

void TimeAxisModel::setTimezone(const QString& tzId) {
    QTimeZone newTz;
    if (tzId == "UTC" || tzId == "utc") {
        newTz = QTimeZone::utc();
    } else if (tzId == "EST" || tzId == "New York" || tzId == "America/New_York") {
        newTz = QTimeZone("America/New_York");
    } else if (tzId == "London" || tzId == "Europe/London") {
        newTz = QTimeZone("Europe/London");
    } else if (tzId == "Tokyo" || tzId == "Asia/Tokyo") {
        newTz = QTimeZone("Asia/Tokyo");
    } else {
        newTz = QTimeZone(tzId.toLatin1());
    }
    
    if (newTz.isValid() && newTz != m_timezone) {
        m_timezone = newTz;
        emit timezoneChanged();
        recalculateTicks();
    }
}

void TimeAxisModel::recalculateTicks() {
    updateTicksAndNotify();
}

bool TimeAxisModel::updateEffectiveViewport() {
    m_effectiveViewportValid = false;
    if (!isViewportValid()) {
        return false;
    }

    const double viewStart = getViewportStart();
    const double viewEnd = getViewportEnd();
    const double viewSpan = viewEnd - viewStart;
    const double viewWidth = getViewportWidth();
    if (viewSpan <= 0.0 || viewWidth <= 0.0) {
        return false;
    }

    m_effectiveStart = viewStart;
    m_effectiveEnd = viewEnd;
    m_effectiveOffsetPx = 0.0;
    m_effectiveSpanPx = viewWidth;
    m_effectiveViewportValid = true;

    if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL")) {
        return true;
    }

    if (auto* grid = renderer()) {
        qint64 dataStart = 0;
        qint64 dataEnd = 0;
        if (grid->heatmapDataTimeRange(dataStart, dataEnd)) {
            const double overlapStart = std::max(viewStart, static_cast<double>(dataStart));
            const double overlapEnd = std::min(viewEnd, static_cast<double>(dataEnd));
            if (overlapEnd <= overlapStart) {
                m_effectiveViewportValid = false;
                return false;
            }
            if (overlapStart > viewStart || overlapEnd < viewEnd) {
                const double ratioStart = (overlapStart - viewStart) / viewSpan;
                const double ratioEnd = (overlapEnd - viewStart) / viewSpan;
                m_effectiveOffsetPx = viewWidth * ratioStart;
                m_effectiveSpanPx = viewWidth * (ratioEnd - ratioStart);
                m_effectiveStart = overlapStart;
                m_effectiveEnd = overlapEnd;
                m_effectiveViewportValid = (m_effectiveSpanPx > 0.0);
            }
        }
    }

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

    if (m_lastNiceStepMs > 0) {
        const double ratio = static_cast<double>(step) / static_cast<double>(m_lastNiceStepMs);
        if (ratio <= 1.2 && ratio >= 0.83) {
            step = m_lastNiceStepMs;
        }
    }
    m_lastNiceStepMs = step;
    qint64 firstTick = (timeStart / step) * step;
    for (qint64 timestamp = firstTick; timestamp <= timeEnd + step; timestamp += step) {
        if (timestamp < timeStart - step) continue;
        double screenX = valueToScreenPosition(static_cast<double>(timestamp));
        if (screenX >= 0 && screenX <= getViewportWidth()) {
            QString label = formatTimeLabel(timestamp, step);
            addTick(static_cast<double>(timestamp), screenX, label, true);
        }
    }
    
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
    if (!m_viewState) {
        return 0.0;
    }
    double start = static_cast<double>(m_viewState->getVisibleTimeStart());
    if (m_viewState->isDragging()) {
        const double viewWidth = getViewportWidth();
        const double timeRange = static_cast<double>(m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
        if (viewWidth > 0.0 && timeRange > 0.0) {
            const double timePixelsToMs = timeRange / viewWidth;
            start += (-m_viewState->getPanVisualOffset().x() * timePixelsToMs);
        }
    }
    return start;
}

double TimeAxisModel::getViewportEnd() const {
    if (!m_viewState) {
        return 60000.0;
    }
    double end = static_cast<double>(m_viewState->getVisibleTimeEnd());
    if (m_viewState->isDragging()) {
        const double viewWidth = getViewportWidth();
        const double timeRange = static_cast<double>(m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
        if (viewWidth > 0.0 && timeRange > 0.0) {
            const double timePixelsToMs = timeRange / viewWidth;
            end += (-m_viewState->getPanVisualOffset().x() * timePixelsToMs);
        }
    }
    return end;
}

double TimeAxisModel::valueToScreenPosition(double value) const {
    if (!isViewportValid()) return 0.0;

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
    QDateTime dateTime = QDateTime::fromMSecsSinceEpoch(timestampMs, m_timezone);
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
