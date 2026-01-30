#include "TimeAxisModel.hpp"
#include "../render/GridViewState.hpp"
#include "../UnifiedGridRenderer.h"
#include <QDebug>
#include <cmath>
#include <algorithm>

// Define nice time steps (in milliseconds) — include sub-100ms for zoomed-in view
const std::vector<TimeAxisModel::TimeStep> TimeAxisModel::TIME_STEPS = {
    {25, "25ms"},
    {50, "50ms"},
    {100, "100ms"},       // 0.1 second
    {250, "250ms"},       // 0.25 second
    {500, "500ms"},       // 0.5 second
    {1000, "1s"},         // 1 second
    {2000, "2s"},         // 2 seconds
    {5000, "5s"},         // 5 seconds
    {10000, "10s"},       // 10 seconds
    {15000, "15s"},       // 15 seconds
    {30000, "30s"},       // 30 seconds
    {60000, "1min"},      // 1 minute
    {120000, "2min"},     // 2 minutes
    {300000, "5min"},     // 5 minutes
    {600000, "10min"},    // 10 minutes
    {900000, "15min"},    // 15 minutes
    {1800000, "30min"},   // 30 minutes
    {3600000, "1h"},      // 1 hour
    {7200000, "2h"},      // 2 hours
    {14400000, "4h"},     // 4 hours
    {21600000, "6h"},     // 6 hours
    {43200000, "12h"},    // 12 hours
    {86400000, "1d"}      // 1 day
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
    
    // Handle common shorthand names
    if (tzId == "UTC" || tzId == "utc") {
        newTz = QTimeZone::utc();
    } else if (tzId == "EST" || tzId == "New York" || tzId == "America/New_York") {
        newTz = QTimeZone("America/New_York");
    } else if (tzId == "London" || tzId == "Europe/London") {
        newTz = QTimeZone("Europe/London");
    } else if (tzId == "Tokyo" || tzId == "Asia/Tokyo") {
        newTz = QTimeZone("Asia/Tokyo");
    } else {
        // Try direct IANA name
        newTz = QTimeZone(tzId.toLatin1());
    }
    
    if (newTz.isValid() && newTz != m_timezone) {
        m_timezone = newTz;
        emit timezoneChanged();
        recalculateTicks();
    }
}

void TimeAxisModel::recalculateTicks() {
    if (isViewportValid()) {
        beginResetModel();
        calculateTicks();
        endResetModel();
    }
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
    
    // Adaptive tick count: when zoomed in to few columns, show more ticks (up to column-level)
    int targetTicks = static_cast<int>(m_effectiveSpanPx / 80.0); // ~80 pixels per tick when zoomed out
    targetTicks = std::max(4, std::min(25, targetTicks));
    // When visible time span is small, allow finer steps (handled by TIME_STEPS and step picker)
    
    qint64 step = calculateNiceTimeStep(timeRange, targetTicks);
    if (step <= 0) return;
    
    // Find first tick at or before timeStart
    qint64 firstTick = (timeStart / step) * step;
    
    // Generate ticks
    for (qint64 timestamp = firstTick; timestamp <= timeEnd + step; timestamp += step) {
        if (timestamp < timeStart - step) continue;
        
        double screenX = valueToScreenPosition(static_cast<double>(timestamp));
        
        // Check if tick is within visible area
        if (screenX >= 0 && screenX <= getViewportWidth()) {
            QString label = formatTimeLabel(timestamp, step);
            bool isMajor = true; // All time ticks are major for now
            
            addTick(static_cast<double>(timestamp), screenX, label, isMajor);
        }
    }
    
    //qDebug() << "TimeAxisModel: Generated" << m_ticks.size() 
    //         << "time ticks for range" << timeRange << "ms, step=" << step << "ms";
}

QString TimeAxisModel::formatLabel(double value) const {
    qint64 timestampMs = static_cast<qint64>(value);
    
    // Use the range to determine appropriate formatting
    qint64 rangeMs = m_effectiveViewportValid
        ? static_cast<qint64>(m_effectiveEnd - m_effectiveStart)
        : static_cast<qint64>(getViewportEnd() - getViewportStart());
    
    return formatTimeLabel(timestampMs, rangeMs);
}

double TimeAxisModel::getViewportStart() const {
    if (!m_viewState) {
        return 0.0;
    }
    double timeStart = static_cast<double>(m_viewState->getVisibleTimeStart());
    if (m_viewState->isDragging()) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const double timeEnd = static_cast<double>(m_viewState->getVisibleTimeEnd());
        const double timeRange = timeEnd - timeStart;
        const double viewportW = getViewportWidth();
        if (!pan.isNull() && timeRange > 0.0 && viewportW > 0.0) {
            const double timePixelsToUnits = timeRange / viewportW;
            timeStart += -pan.x() * timePixelsToUnits;
        }
    }
    return timeStart;
}

double TimeAxisModel::getViewportEnd() const {
    if (!m_viewState) {
        return 60000.0;
    }
    double timeEnd = static_cast<double>(m_viewState->getVisibleTimeEnd());
    if (m_viewState->isDragging()) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const double timeStart = static_cast<double>(m_viewState->getVisibleTimeStart());
        const double timeRange = timeEnd - timeStart;
        const double viewportW = getViewportWidth();
        if (!pan.isNull() && timeRange > 0.0 && viewportW > 0.0) {
            const double timePixelsToUnits = timeRange / viewportW;
            timeEnd += -pan.x() * timePixelsToUnits;
        }
    }
    return timeEnd;
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
    
    // Time axis is horizontal - earlier times at left
    double normalized = (value - timeStart) / (timeEnd - timeStart);
    
    return normalized * getViewportWidth();
}

qint64 TimeAxisModel::calculateNiceTimeStep(qint64 rangeMs, int targetTicks) const {
    if (rangeMs <= 0 || targetTicks <= 0) return 1000; // Default 1 second
    
    qint64 rawStep = rangeMs / targetTicks;
    
    // Find the best matching time step
    auto it = std::lower_bound(TIME_STEPS.begin(), TIME_STEPS.end(), rawStep,
        [](const TimeStep& step, qint64 value) {
            return step.milliseconds < value;
        });
    
    if (it == TIME_STEPS.end()) {
        // Use the largest step
        return TIME_STEPS.back().milliseconds;
    } else if (it == TIME_STEPS.begin()) {
        // Use the smallest step
        return TIME_STEPS.front().milliseconds;
    } else {
        // Choose between current and previous step based on which is closer
        auto prev = it - 1;
        qint64 currDiff = std::abs(it->milliseconds - rawStep);
        qint64 prevDiff = std::abs(prev->milliseconds - rawStep);
        
        return (currDiff < prevDiff) ? it->milliseconds : prev->milliseconds;
    }
}

QString TimeAxisModel::formatTimeLabel(qint64 timestampMs, qint64 stepMs) const {
    QDateTime dateTime = QDateTime::fromMSecsSinceEpoch(timestampMs, m_timezone);
    
    // TradingView-style adaptive formatting:
    // Show minimal info, add context only at boundaries
    
    if (stepMs < 100) {
        // Sub-100ms: show ms only, second context at second boundaries
        int ms = dateTime.time().msec();
        int second = dateTime.time().second();
        if (second == 0 && ms == 0) {
            return dateTime.toString(":ss");
        }
        return QString(".%1").arg(ms, 3, 10, QChar('0'));
    } else if (stepMs >= 86400000) {
        // Day scale or larger - show day of month, month at boundaries
        int day = dateTime.date().day();
        if (day == 1) {
            // Month boundary - show month name
            return dateTime.toString("MMM");
        }
        return QString::number(day);
        
    } else if (stepMs >= 3600000) {
        // Hour scale - show hour, date at day boundaries
        int hour = dateTime.time().hour();
        if (hour == 0) {
            // Day boundary - show date
            return dateTime.toString("d MMM");
        }
        return dateTime.toString("HH:mm");
        
    } else if (stepMs >= 60000) {
        // Minute scale - show HH:MM, hour context at hour boundaries
        int minute = dateTime.time().minute();
        if (minute == 0) {
            // Hour boundary - show full time
            return dateTime.toString("HH:mm");
        }
        return dateTime.toString(":mm");
        
    } else if (stepMs >= 1000) {
        // Second scale - show :SS, minute context at minute boundaries
        int second = dateTime.time().second();
        if (second == 0) {
            // Minute boundary
            return dateTime.toString("HH:mm");
        }
        return dateTime.toString(":ss");
        
    } else {
        // Sub-second - show .ms, second context at second boundaries
        int ms = dateTime.time().msec();
        if (ms == 0) {
            // Second boundary
            return dateTime.toString(":ss");
        }
        return QString(".%1").arg(ms, 3, 10, QChar('0'));
    }
}
