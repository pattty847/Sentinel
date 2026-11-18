#include "DisplayTime.hpp"
#include <QCoreApplication>
#include <cstdlib>
#include <utility>

namespace {
qint64 floorDiv(qint64 a, qint64 b) {
    const qint64 q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}
qint64 floorMod(qint64 a, qint64 b) { return a - floorDiv(a, b) * b; }
// The first date on or after `d` that starts a period of `count` units.
QDate periodStartOnOrAfter(QDate d, DisplayTime::Calendar unit, int count) {
    switch (unit) {
    case DisplayTime::Calendar::Days:
        return d.addDays((count - floorMod(d.toJulianDay(), count)) % count);
    case DisplayTime::Calendar::Weeks: {
        const QDate monday = d.addDays((8 - d.dayOfWeek()) % 7); // Julian day % 7 == 0
        return monday.addDays(7 * ((count - floorMod(monday.toJulianDay() / 7, count)) % count));
    }
    case DisplayTime::Calendar::Months: {
        const QDate first = d.day() == 1 ? d : QDate(d.year(), d.month(), 1).addMonths(1);
        const qint64 month = qint64(first.year()) * 12 + first.month() - 1;
        return first.addMonths(int((count - floorMod(month, count)) % count));
    }
    }
    return d;
}
} // namespace

DisplayTime& DisplayTime::instance() {
    static DisplayTime time;
    return time;
}

DisplayTime::DisplayTime() : m_systemZoneId([] { return QTimeZone::systemTimeZoneId(); }) {
    adoptSystemZone();
    m_systemPoll.setInterval(1000);
    connect(&m_systemPoll, &QTimer::timeout, this, &DisplayTime::refreshSystemZone);
}

void DisplayTime::setZone(Zone zone) {
    if (m_zone == zone) return;
    m_zone = zone;
    if (m_zone == Zone::Local) adoptSystemZone(); // the zone the computer has now
    m_cacheValid = false;
    // Only Local mode reads the system zone, so only Local mode watches it.
    if (m_zone == Zone::Utc) m_systemPoll.stop();
    else if (auto *app = QCoreApplication::instance()) { // no timers without an event loop (plain unit tests)
        if (!m_quitHooked) m_quitHooked = bool(connect(app, &QCoreApplication::aboutToQuit, &m_systemPoll, &QTimer::stop));
        m_systemPoll.start();
    }
    emit zoneChanged();
}

void DisplayTime::setLocalZone(const QTimeZone& zone) {
    const QTimeZone before = m_local;
    m_injected = zone.isValid();
    if (m_injected) m_local = zone;
    else { m_systemId.clear(); adoptSystemZone(); }
    if (m_local == before) return;
    m_cacheValid = false;
    if (m_zone == Zone::Local) emit zoneChanged();
}

void DisplayTime::setSystemZoneIdSource(std::function<QByteArray()> source) {
    m_systemZoneId = source ? std::move(source) : [] { return QTimeZone::systemTimeZoneId(); };
}

bool DisplayTime::adoptSystemZone() {
    if (m_injected) return false;
    const QByteArray id = m_systemZoneId();
    if (id == m_systemId && m_local.isValid()) return false;
    m_systemId = id;
    const QTimeZone zone(id);
    m_local = zone.isValid() ? zone : QTimeZone::systemTimeZone();
    m_cacheValid = false;
    return true;
}

bool DisplayTime::refreshSystemZone() {
    if (!adoptSystemZone()) return false;
    if (m_zone == Zone::Local) emit zoneChanged();
    return true;
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
    // previousTransition is strictly before its argument, and a backend may compare in
    // whole seconds: ask one second later and accept the transition only if it is not
    // after `ms` and leads into this offset (else the segment starts at `ms`, which is
    // still correct). A later one can keep the offset (a DST relabel, Lisbon 1992).
    const auto previous = m_local.previousTransition(utc.addSecs(1));
    segment.from = previous.atUtc.isValid()
        ? segmentStart(ms, segment.offsetSeconds, previous.atUtc.toMSecsSinceEpoch(), previous.offsetFromUtc) : ms;
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

qint64 DisplayTime::nextCalendarBoundary(qint64 afterMs, Calendar unit, int count) const {
    if (count <= 0) return afterMs + 1;
    if (afterMs > kBoundaryLimitMs || afterMs < -kBoundaryLimitMs) return std::numeric_limits<qint64>::max();
    const QTimeZone zone = m_zone == Zone::Utc ? QTimeZone::utc() : m_local;
    QDate date = at(afterMs).date();
    for (int tries = 0; tries < 4; ++tries) {
        const QDate start = periodStartOnOrAfter(date, unit, count);
        // startOfDay: the first instant of that date, also where a DST change skips midnight.
        const qint64 ms = start.startOfDay(zone).toMSecsSinceEpoch();
        if (ms > afterMs) return ms;
        date = start.addDays(1);
    }
    return std::numeric_limits<qint64>::max();
}

QString DisplayTime::rangeText(qint64 startMs, qint64 endMs, const QString& pattern) const {
    const QString start = format(startMs, pattern), end = format(endMs, pattern);
    if (offsetSeconds(startMs) == offsetSeconds(endMs))
        return start + QStringLiteral(" to ") + end + QLatin1Char(' ') + zoneLabel(endMs);
    return start + QLatin1Char(' ') + zoneLabel(startMs) + QStringLiteral(" to ") + end + QLatin1Char(' ') + zoneLabel(endMs);
}

qint64 DisplayTime::nextBoundary(qint64 afterMs, qint64 stepMs) const {
    if (stepMs <= 0) return afterMs + 1;
    if (afterMs > kBoundaryLimitMs || afterMs < -kBoundaryLimitMs || stepMs > kBoundaryLimitMs) return std::numeric_limits<qint64>::max();
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
