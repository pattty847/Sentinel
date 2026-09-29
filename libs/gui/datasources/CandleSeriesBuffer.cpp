/*
Sentinel — CandleSeriesBuffer
*/
#include "CandleSeriesBuffer.hpp"
#include "SentinelLogging.hpp"

#include <algorithm>

CandleSeriesBuffer::CandleSeriesBuffer(QObject* parent)
    : QObject(parent) {
}

size_t CandleSeriesBuffer::capacityFor(int64_t timeframeSec) {
    if (timeframeSec <= 1) return 60000;
    if (timeframeSec <= 60) return 20000;
    return 10000;
}

const CandleSeriesBuffer::CandleBar& CandleSeriesBuffer::getAt(const Series& series, size_t index) {
    return series.ring[(series.head + index) % series.capacity];
}

CandleSeriesBuffer::CandleBar& CandleSeriesBuffer::getAt(Series& series, size_t index) {
    return series.ring[(series.head + index) % series.capacity];
}

size_t CandleSeriesBuffer::lowerBound(const Series& series, qint64 timeStartMs) {
    size_t lo = 0;
    size_t hi = series.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (getAt(series, mid).timeStartMs < timeStartMs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t CandleSeriesBuffer::upperBound(const Series& series, qint64 timeEndMs) {
    size_t lo = 0;
    size_t hi = series.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (getAt(series, mid).timeStartMs <= timeEndMs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void CandleSeriesBuffer::applyUpdate(const QString& symbol,
                                     int64_t timeframeSec,
                                     const CandleBar& bar,
                                     int64_t seq,
                                     bool isClosed) {
    if (symbol.isEmpty() || timeframeSec <= 0) {
        return;
    }

    auto& series = seriesFor(symbol, timeframeSec);

    if (seq <= series.lastSeq) {
        sLog_Probe("candles.drop",
                   "stale seq symbol=" << symbol << " tfSec=" << timeframeSec
                   << " seq=" << seq << " lastSeq=" << series.lastSeq
                   << " barStartMs=" << bar.timeStartMs);
        return;
    }
    series.lastSeq = seq;

    CandleBar updated = bar;
    updated.isClosed = isClosed || bar.isClosed;
    updated.seq = seq;

    bool updatedExisting = false;
    if (series.count > 0) {
        size_t idx = lowerBound(series, updated.timeStartMs);
        if (idx < series.count && getAt(series, idx).timeStartMs == updated.timeStartMs) {
            CandleBar& existing = getAt(series, idx);
            if (!existing.isClosed) {
                existing = updated;
            }
            updatedExisting = true;
        }
    }

    const bool olderThanNewest = series.count > 0 &&
        updated.timeStartMs < getAt(series, series.count - 1).timeStartMs;
    if (!updatedExisting && olderThanNewest) {
        // Rare late bar: keep the ring sorted so lowerBound stays valid.
        std::vector<CandleBar> bars = linearize(series);
        bars.insert(bars.begin() + static_cast<std::ptrdiff_t>(lowerBound(series, updated.timeStartMs)), updated);
        rebuild(series, std::move(bars));
    } else if (!updatedExisting) {
        if (series.count < series.capacity) {
            getAt(series, series.count) = updated;
            series.count++;
        } else {
            series.ring[series.head] = updated;
            series.head = (series.head + 1) % series.capacity;
        }
    }

    emit candlesDirty(symbol, timeframeSec, updated.timeStartMs, updated.timeEndMs);

    sLog_Probe("candles.buffer",
               "symbol=" << symbol << " tfSec=" << timeframeSec
               << " seq=" << seq << " barStartMs=" << updated.timeStartMs
               << " replaced=" << updatedExisting
               << " count=" << series.count
               << " oldestMs=" << getAt(series, 0).timeStartMs
               << " newestMs=" << getAt(series, series.count - 1).timeStartMs);
}

CandleSeriesBuffer::Series& CandleSeriesBuffer::seriesFor(const QString& symbol, int64_t timeframeSec) {
    auto& series = m_series[SeriesKey{symbol, timeframeSec}];
    if (series.capacity == 0) {
        series.capacity = capacityFor(timeframeSec);
        series.ring.resize(series.capacity);
    }
    return series;
}

std::vector<CandleSeriesBuffer::CandleBar> CandleSeriesBuffer::linearize(const Series& series) {
    std::vector<CandleBar> bars;
    bars.reserve(series.count + 1);
    for (size_t i = 0; i < series.count; ++i) {
        bars.push_back(getAt(series, i));
    }
    return bars;
}

void CandleSeriesBuffer::rebuild(Series& series, std::vector<CandleBar>&& sorted) {
    if (sorted.size() > series.capacity) {
        sorted.erase(sorted.begin(), sorted.end() - static_cast<std::ptrdiff_t>(series.capacity));
    }
    std::copy(sorted.begin(), sorted.end(), series.ring.begin());
    series.head = 0;
    series.count = sorted.size();
}

void CandleSeriesBuffer::applyHistory(const QString& symbol,
                                      int64_t timeframeSec,
                                      const std::vector<CandleBar>& history) {
    if (symbol.isEmpty() || timeframeSec <= 0 || history.empty()) {
        return;
    }
    auto& series = seriesFor(symbol, timeframeSec);
    std::vector<CandleBar> page = history;
    page.erase(std::remove_if(page.begin(), page.end(), [](const CandleBar& bar) {
        return bar.timeStartMs <= 0;
    }), page.end());
    std::stable_sort(page.begin(), page.end(), [](const CandleBar& a, const CandleBar& b) {
        return a.timeStartMs < b.timeStartMs;
    });
    // Merge once rather than shifting the entire retained series per older bar.
    std::vector<CandleBar> merged;
    merged.reserve(series.count + page.size());
    size_t existing = 0;
    for (const auto& bar : page) {
        while (existing < series.count && getAt(series, existing).timeStartMs <= bar.timeStartMs)
            merged.push_back(getAt(series, existing++));
        if (!merged.empty() && merged.back().timeStartMs == bar.timeStartMs) {
            // Preserve live values, except a final history bucket can close an open one.
            if (bar.isClosed && !merged.back().isClosed) merged.back() = bar;
        } else {
            merged.push_back(bar);
        }
    }
    while (existing < series.count) merged.push_back(getAt(series, existing++));
    rebuild(series, std::move(merged));
    if (series.count == 0) {
        return;
    }
    const qint64 first = getAt(series, 0).timeStartMs;
    const qint64 last = getAt(series, series.count - 1).timeEndMs;
    emit candlesDirty(symbol, timeframeSec, first, last);
    sLog_Probe("candles.buffer",
               "history symbol=" << symbol << " tfSec=" << timeframeSec
               << " merged=" << history.size() << " count=" << series.count
               << " oldestMs=" << first << " newestMs=" << getAt(series, series.count - 1).timeStartMs
               << " lastSeq=" << series.lastSeq);
}

void CandleSeriesBuffer::resetSequences() {
    for (auto& [key, series] : m_series) {
        series.lastSeq = 0;
    }
}

qint64 CandleSeriesBuffer::oldestTimeMs(const QString& symbol, int64_t timeframeSec) const {
    const auto it = m_series.find(SeriesKey{symbol, timeframeSec});
    return it == m_series.end() || it->second.count == 0 ? 0 : getAt(it->second, 0).timeStartMs;
}

bool CandleSeriesBuffer::historyCapacityReached(const QString& symbol, int64_t timeframeSec) const {
    const auto it = m_series.find(SeriesKey{symbol, timeframeSec});
    return it != m_series.end() && it->second.count == it->second.capacity;
}

bool CandleSeriesBuffer::getVisibleSlice(const QString& symbol,
                                         int64_t timeframeSec,
                                         qint64 timeStartMs,
                                         qint64 timeEndMs,
                                         std::vector<CandleBar>& out) const {
    out.clear();
    if (symbol.isEmpty() || timeframeSec <= 0 || timeEndMs <= timeStartMs) {
        return false;
    }

    SeriesKey key{symbol, timeframeSec};
    auto it = m_series.find(key);
    if (it == m_series.end()) {
        return false;
    }

    const Series& series = it->second;
    if (series.count == 0) {
        return false;
    }

    size_t start = lowerBound(series, timeStartMs);
    size_t end = upperBound(series, timeEndMs);
    if (start >= end || start >= series.count) {
        return false;
    }

    const size_t count = std::min(end, series.count) - start;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(getAt(series, start + i));
    }
    return !out.empty();
}

bool CandleSeriesBuffer::getBoundedSlice(const QString& symbol, int64_t timeframeSec,
                                         qint64 startMs, qint64 endMs, size_t limit,
                                         std::vector<CandleBar>& out, bool& hasMore,
                                         qint64& nextStartMs) const {
    out.clear();
    hasMore = false;
    nextStartMs = 0;
    if (symbol.isEmpty() || timeframeSec <= 0 || startMs >= endMs || limit == 0) return false;
    const auto it = m_series.find(SeriesKey{symbol, timeframeSec});
    if (it == m_series.end()) return false;
    const Series& series = it->second;
    const size_t first = lowerBound(series, startMs);
    const size_t last = lowerBound(series, endMs); // API end is exclusive.
    if (first >= last) return true;
    const size_t available = last - first;
    const size_t count = std::min(available, limit);
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back(getAt(series, first + i));
    hasMore = available > count;
    if (hasMore) nextStartMs = getAt(series, first + count).timeStartMs;
    return true;
}
