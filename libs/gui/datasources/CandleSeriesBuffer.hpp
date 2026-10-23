/*
Sentinel — CandleSeriesBuffer
Role: GUI-thread candle ring buffer for live candle updates.
Threading: GUI thread only.
*/
#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include "CandleCoverage.hpp"
#include <unordered_map>
#include <vector>

class CandleSeriesBuffer : public QObject {
    Q_OBJECT
public:
    struct CandleBar {
        qint64 timeStartMs = 0;
        qint64 timeEndMs = 0;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        double volume = 0.0;
        bool isClosed = false;
        int64_t seq = 0;
        bool historyRefreshable = false; // cached across a session/selection boundary
    };

    explicit CandleSeriesBuffer(QObject* parent = nullptr);

    void applyUpdate(const QString& symbol,
                     int64_t timeframeMs,
                     const CandleBar& bar,
                     int64_t seq,
                     bool isClosed);

    // Merges history by start time. Live-owned buckets always win, including
    // against "closed" REST snapshots; the live seq stream is not touched.
    // The page covers [coverStartMs, coverEndMs) (its wire window, also when it
    // is empty) and at least the span from its first to its last bar.
    void applyHistory(const QString& symbol,
                      int64_t timeframeMs,
                      const std::vector<CandleBar>& bars,
                      qint64 coverStartMs = 0,
                      qint64 coverEndMs = 0);

    // New sessions/selections revoke all cached live ownership. Fresh history
    // can correct closed cached bars; the new live stream re-establishes ownership.
    void resetSequences();
    void resetSeriesForSelection(const QString& symbol, int64_t timeframeMs);
    // The live stream for this series (or for every series) stopped; its coverage ends at the last live bar.
    void endLiveCoverage(const QString& symbol, int64_t timeframeMs);
    void endLiveCoverage();
    // History reached the live edge at liveFromMs before a live bar: the selected
    // stream covers the time after it until the stream ends.
    void openLiveCoverage(const QString& symbol, int64_t timeframeMs, qint64 liveFromMs);

    // Fetched time of this series; nullptr when it has no series. Does not allocate.
    const CandleCoverage* coverage(const QString& symbol, int64_t timeframeMs) const;

    qint64 oldestTimeMs(const QString& symbol, int64_t timeframeMs) const;
    bool historyCapacityReached(const QString& symbol, int64_t timeframeMs) const;

    bool getVisibleSlice(const QString& symbol,
                         int64_t timeframeMs,
                         qint64 timeStartMs,
                         qint64 timeEndMs,
                         std::vector<CandleBar>& out) const;

    // Copies at most limit bars for API pagination. The end is exclusive.
    // Returns false when this symbol/timeframe has no local series.
    bool getBoundedSlice(const QString& symbol, int64_t timeframeMs,
                         qint64 startMs, qint64 endMs, size_t limit,
                         std::vector<CandleBar>& out, bool& hasMore,
                         qint64& nextStartMs) const;

signals:
    void candlesDirty(const QString& symbol,
                      int64_t timeframeMs,
                      qint64 dirtyStartMs,
                      qint64 dirtyEndMs);

private:
    struct SeriesKey {
        QString symbol;
        int64_t timeframeMs = 0;
        bool operator==(const SeriesKey& other) const {
            return timeframeMs == other.timeframeMs && symbol == other.symbol;
        }
    };

    struct SeriesKeyHash {
        size_t operator()(const SeriesKey& key) const noexcept {
            return qHash(key.symbol) ^ (static_cast<size_t>(key.timeframeMs) << 1);
        }
    };

    struct Series {
        std::vector<CandleBar> ring;
        size_t head = 0;
        size_t count = 0;
        size_t capacity = 0;
        int64_t lastSeq = 0;
        CandleCoverage coverage;
    };

    static size_t capacityFor(int64_t timeframeMs);
    static void resetOwnership(Series& series);
    static const CandleBar& getAt(const Series& series, size_t index);
    static CandleBar& getAt(Series& series, size_t index);
    static size_t lowerBound(const Series& series, qint64 timeStartMs);
    static size_t upperBound(const Series& series, qint64 timeEndMs);
    static std::vector<CandleBar> linearize(const Series& series);
    // Replaces the ring with time-sorted bars, keeping the newest `capacity`.
    static void rebuild(Series& series, std::vector<CandleBar>&& sorted);
    static void trimEvictedCoverage(Series& series);
    static void limitBarlessCoverage(Series& series);
    Series& seriesFor(const QString& symbol, int64_t timeframeMs);

    std::unordered_map<SeriesKey, Series, SeriesKeyHash> m_series;
};
