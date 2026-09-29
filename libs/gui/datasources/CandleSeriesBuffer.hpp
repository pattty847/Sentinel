/*
Sentinel — CandleSeriesBuffer
Role: GUI-thread candle ring buffer for live candle updates.
Threading: GUI thread only.
*/
#pragma once

#include <QObject>
#include <QString>
#include <QHash>
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
    };

    explicit CandleSeriesBuffer(QObject* parent = nullptr);

    void applyUpdate(const QString& symbol,
                     int64_t timeframeSec,
                     const CandleBar& bar,
                     int64_t seq,
                     bool isClosed);

    // Merges history by start time. Live-owned buckets always win, including
    // against "closed" REST snapshots; the live seq stream is not touched.
    void applyHistory(const QString& symbol,
                      int64_t timeframeSec,
                      const std::vector<CandleBar>& bars);

    // Live seq numbers restart with each server session; call on (re)connect.
    void resetSequences();

    qint64 oldestTimeMs(const QString& symbol, int64_t timeframeSec) const;
    bool historyCapacityReached(const QString& symbol, int64_t timeframeSec) const;

    bool getVisibleSlice(const QString& symbol,
                         int64_t timeframeSec,
                         qint64 timeStartMs,
                         qint64 timeEndMs,
                         std::vector<CandleBar>& out) const;

    // Copies at most limit bars for API pagination. The end is exclusive.
    // Returns false when this symbol/timeframe has no local series.
    bool getBoundedSlice(const QString& symbol, int64_t timeframeSec,
                         qint64 startMs, qint64 endMs, size_t limit,
                         std::vector<CandleBar>& out, bool& hasMore,
                         qint64& nextStartMs) const;

signals:
    void candlesDirty(const QString& symbol,
                      int64_t timeframeSec,
                      qint64 dirtyStartMs,
                      qint64 dirtyEndMs);

private:
    struct SeriesKey {
        QString symbol;
        int64_t timeframeSec = 0;
        bool operator==(const SeriesKey& other) const {
            return timeframeSec == other.timeframeSec && symbol == other.symbol;
        }
    };

    struct SeriesKeyHash {
        size_t operator()(const SeriesKey& key) const noexcept {
            return qHash(key.symbol) ^ (static_cast<size_t>(key.timeframeSec) << 1);
        }
    };

    struct Series {
        std::vector<CandleBar> ring;
        size_t head = 0;
        size_t count = 0;
        size_t capacity = 0;
        int64_t lastSeq = 0;
    };

    static size_t capacityFor(int64_t timeframeSec);
    static const CandleBar& getAt(const Series& series, size_t index);
    static CandleBar& getAt(Series& series, size_t index);
    static size_t lowerBound(const Series& series, qint64 timeStartMs);
    static size_t upperBound(const Series& series, qint64 timeEndMs);
    static std::vector<CandleBar> linearize(const Series& series);
    // Replaces the ring with time-sorted bars, keeping the newest `capacity`.
    static void rebuild(Series& series, std::vector<CandleBar>&& sorted);
    Series& seriesFor(const QString& symbol, int64_t timeframeSec);

    std::unordered_map<SeriesKey, Series, SeriesKeyHash> m_series;
};
