#pragma once
// The one time zone every chart time label uses (chart setting `timeZone`): UTC, the
// default, or the machine's local zone with its daylight-saving rules. Every label a
// user reads in the chart converts epoch milliseconds through here: the time-axis
// tick labels and the boundaries their grid lands on, the CVD "since" text and the
// heatmap resolution notice. Only labels change. What the data means stays UTC:
// candle buckets, TPO and CVD session boundaries, recorded times, server requests,
// logs and file names. A UTC-defined boundary keeps its instant and is only shown
// in the active zone.
//
// Process-wide like PriceUnits; HeatmapChartControls applies the chart setting.
// GUI thread only (the transition cache is not locked).
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimeZone>
#include <cstdint>
#include <limits>
#include <string_view>

class DisplayTime : public QObject {
    Q_OBJECT
public:
    enum class Zone { Utc, Local };
    static DisplayTime& instance();
    // The setting's value: "local" is Local, anything else UTC.
    static Zone parse(std::string_view value) { return value == "local" ? Zone::Local : Zone::Utc; }

    Zone zone() const { return m_zone; }
    void setZone(Zone zone);
    // The zone Local mode uses; the system zone by default. Tests inject a fixed zone
    // so they never depend on the machine's; an invalid zone restores the system one.
    void setLocalZone(const QTimeZone& zone);

    // `ms` in the active zone.
    QDateTime at(qint64 ms) const;
    QString format(qint64 ms, const QString& pattern) const { return at(ms).toString(pattern); }
    // The offset from UTC at `ms`, in seconds (0 in UTC mode).
    int offsetSeconds(qint64 ms) const { return segmentAt(ms).offsetSeconds; }
    // The zone as a label states it: "UTC", or the local offset at `ms` ("UTC-4",
    // "UTC+5:30"). An offset, not an abbreviation: platform abbreviations differ
    // ("IST" or "GMT+5:30") and an offset always says which instant is meant.
    QString zoneLabel(qint64 ms) const;
    // The first instant after `afterMs` whose wall clock in the active zone is a whole
    // multiple of `stepMs` from midnight (`stepMs` divides a day or is one day), so a
    // 1h grid lands on local hours and a 1d grid on local midnights, also across a
    // daylight-saving change and in 30- or 45-minute offset zones. In UTC mode it is
    // the next multiple of `stepMs` since the epoch.
    qint64 nextBoundary(qint64 afterMs, qint64 stepMs) const;

signals:
    void zoneChanged();

private:
    DisplayTime();
    struct Segment {
        qint64 from = std::numeric_limits<qint64>::min(); // first instant with this offset
        qint64 to = std::numeric_limits<qint64>::max();   // the next transition (exclusive)
        int offsetSeconds = 0;
    };
    Segment segmentAt(qint64 ms) const;
    Zone m_zone = Zone::Utc;
    QTimeZone m_local;
    mutable Segment m_cache;
    mutable bool m_cacheValid = false;
};
