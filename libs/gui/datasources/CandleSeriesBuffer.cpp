/*
Sentinel — CandleSeriesBuffer
*/
#include "CandleSeriesBuffer.hpp"

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

    SeriesKey key{symbol, timeframeSec};
    auto& series = m_series[key];
    if (series.capacity == 0) {
        series.capacity = capacityFor(timeframeSec);
        series.ring.resize(series.capacity);
    }

    if (seq <= series.lastSeq) {
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

    if (!updatedExisting) {
        if (series.count < series.capacity) {
            getAt(series, series.count) = updated;
            series.count++;
        } else {
            series.ring[series.head] = updated;
            series.head = (series.head + 1) % series.capacity;
        }
    }

    emit candlesDirty(symbol, timeframeSec, updated.timeStartMs, updated.timeEndMs);
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
