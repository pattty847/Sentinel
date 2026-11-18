#include "DisplayTime.hpp"
#include <cstdlib>

namespace {
qint64 floorDiv(qint64 a, qint64 b) {
    const qint64 q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}
} // namespace

DisplayTime& DisplayTime::instance() {
    static DisplayTime time;
    return time;
}

DisplayTime::DisplayTime() : m_local(QTimeZone::systemTimeZone()) {}

void DisplayTime::setZone(Zone zone) {
    if (m_zone == zone) return;
    m_zone = zone;
    m_cacheValid = false;
    emit zoneChanged();
}

void DisplayTime::setLocalZone(const QTimeZone& zone) {
    const QTimeZone next = zone.isValid() ? zone : QTimeZone::systemTimeZone();
    if (next == m_local) return;
    m_local = next;
    m_cacheValid = false;
    if (m_zone == Zone::Local) emit zoneChanged();
}

QDateTime DisplayTime::at(qint64 ms) const {
    return m_zone == Zone::Utc ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
                               : QDateTime::fromMSecsSinceEpoch(ms, m_local);
}

DisplayTime::Segment DisplayTime::segmentAt(qint64 ms) const {
    if (m_zone == Zone::Utc) return {};
    if (m_cacheValid && ms >= m_cache.from && ms < m_cache.to) return m_cache;
    const QDateTime utc = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
    Segment segment;
    segment.offsetSeconds = m_local.offsetFromUtc(utc);
    if (!m_local.hasTransitions()) return segment; // a fixed offset, or a backend without rules
    // previousTransition is strictly before its argument: one millisecond later
    // includes a transition at exactly `ms`.
    const auto previous = m_local.previousTransition(utc.addMSecs(1));
    if (previous.atUtc.isValid()) segment.from = previous.atUtc.toMSecsSinceEpoch();
    const auto next = m_local.nextTransition(utc);
    if (next.atUtc.isValid()) segment.to = next.atUtc.toMSecsSinceEpoch();
    if (ms < segment.from || ms >= segment.to) return {ms, ms + 1, segment.offsetSeconds}; // inconsistent rules: no cache
    m_cache = segment;
    m_cacheValid = true;
    return segment;
}

QString DisplayTime::zoneLabel(qint64 ms) const {
    if (m_zone == Zone::Utc) return QStringLiteral("UTC");
    const int offset = offsetSeconds(ms);
    if (offset == 0) return QStringLiteral("UTC");
    const int minutes = std::abs(offset) / 60;
    QString label = QStringLiteral("UTC") + (offset < 0 ? QLatin1Char('-') : QLatin1Char('+')) + QString::number(minutes / 60);
    if (minutes % 60) label += QStringLiteral(":%1").arg(minutes % 60, 2, 10, QLatin1Char('0'));
    return label;
}

qint64 DisplayTime::nextBoundary(qint64 afterMs, qint64 stepMs) const {
    if (stepMs <= 0) return afterMs + 1;
    qint64 from = afterMs + 1;
    // Within one offset segment the boundaries are k * step - offset; a candidate past
    // the segment's end belongs to the next offset, so start again there.
    for (int segments = 0; segments < 8; ++segments) {
        const Segment segment = segmentAt(from);
        const qint64 offset = qint64(segment.offsetSeconds) * 1000;
        const qint64 candidate = -floorDiv(-(from + offset), stepMs) * stepMs - offset;
        if (candidate < segment.to) return candidate;
        from = segment.to;
    }
    return from;
}
