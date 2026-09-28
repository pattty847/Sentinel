#include "HeatmapColumnWindow.hpp"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace heatmap_window {
namespace {

constexpr int64_t kNoBucket = std::numeric_limits<int64_t>::min();
// "Everything older than this" without overflowing tf arithmetic.
constexpr int64_t kFarPast = std::numeric_limits<int64_t>::min() / 4;
constexpr int kMaxWidth = 16384;  // Metal texture limit per side

bool nearlyEqual(double a, double b) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= 1e-9 * scale;
}

uint16_t readCell(const QByteArray& bytes, int row, int bytesPerCell) {
    if (bytesPerCell == 1) {
        return static_cast<uint8_t>(bytes.at(row));
    }
    uint16_t value = 0;
    std::memcpy(&value, bytes.constData() + row * 2, sizeof(value));
    return qFromLittleEndian(value);
}

void writeCell(QByteArray& bytes, int row, int bytesPerCell, uint16_t value) {
    if (bytesPerCell == 1) {
        bytes[row] = static_cast<char>(value);
        return;
    }
    const uint16_t le = qToLittleEndian(value);
    std::memcpy(bytes.data() + row * 2, &le, sizeof(le));
}

Band bandOf(const Column& column) {
    return {column.minPrice, column.maxPrice, column.tickSize};
}

} // namespace

bool Band::sameAs(const Band& other) const {
    return nearlyEqual(minPrice, other.minPrice) &&
           nearlyEqual(maxPrice, other.maxPrice) &&
           nearlyEqual(tickSize, other.tickSize);
}

int intensityMagnitude(uint16_t value, int bytesPerCell) {
    if (value == 0) {
        return 0;
    }
    const uint16_t askBase = (bytesPerCell == 1) ? 128 : 0x8000;
    return (value >= askBase) ? value - askBase : value;
}

void resampleColumn(const Column& source, const Band& target, int rows, int bytesPerCell,
                    QByteArray& outIntensity, QByteArray& outLiquidity) {
    const int intensityBytes = rows * bytesPerCell;
    const int liquidityBytes = rows * static_cast<int>(sizeof(uint16_t));
    const bool hasLiquidity = source.liquidity.size() == liquidityBytes;
    const Band sourceBand = bandOf(source);

    // Same band: share the bytes (implicitly shared QByteArray, no copy).
    if (sourceBand.sameAs(target) && source.intensity.size() == intensityBytes) {
        outIntensity = source.intensity;
        outLiquidity = hasLiquidity ? source.liquidity : QByteArray();
        return;
    }

    outIntensity = QByteArray(intensityBytes, 0);
    outLiquidity = hasLiquidity ? QByteArray(liquidityBytes, 0) : QByteArray();
    if (source.intensity.size() != intensityBytes || !sourceBand.valid() || !target.valid()) {
        return;
    }

    for (int row = 0; row < rows; ++row) {
        const double price = source.maxPrice - (static_cast<double>(row) + 0.5) * source.tickSize;
        const int targetRow = static_cast<int>(std::floor((target.maxPrice - price) / target.tickSize));
        if (targetRow < 0 || targetRow >= rows) {
            continue;
        }
        const uint16_t incoming = readCell(source.intensity, row, bytesPerCell);
        if (incoming != 0) {
            const uint16_t existing = readCell(outIntensity, targetRow, bytesPerCell);
            if (existing == 0 ||
                intensityMagnitude(incoming, bytesPerCell) > intensityMagnitude(existing, bytesPerCell)) {
                writeCell(outIntensity, targetRow, bytesPerCell, incoming);
            }
        }
        if (hasLiquidity) {
            const uint16_t incomingLiquidity = readCell(source.liquidity, row, 2);
            if (incomingLiquidity > readCell(outLiquidity, targetRow, 2)) {
                writeCell(outLiquidity, targetRow, 2, incomingLiquidity);
            }
        }
    }
}

void ColumnWindow::configure(int64_t timeframeMs, int width, int cacheCapacity) {
    m_timeframeMs = std::max<int64_t>(1, timeframeMs);
    m_width = std::clamp(width, 1, kMaxWidth);
    m_capacity = std::max(cacheCapacity, m_width + kPageColumns);
    clear();
}

void ColumnWindow::clear() {
    m_rows = 0;
    m_bytesPerCell = 0;
    m_cache.clear();
    m_projected.clear();
    m_recording = false;
    m_bandGeneration = 0;
    m_requestId.clear();
    m_latestRecordingMs = 0;
    m_sizeFloor = m_codesPerOctave = 0.0;
    m_known.clear();
    m_floorMs = 0;
    m_latestLiveMs = 0;
    m_placed = false;
    m_pinned = false;
    m_windowEndMs = 0;
    m_band = {};
    m_zeroIntensity.clear();
    m_zeroValidity.clear();
    m_slotBucket.assign(static_cast<size_t>(std::max(0, m_width)), kNoBucket);
    m_slotRecorded.assign(static_cast<size_t>(std::max(0, m_width)), 0);
}

int ColumnWindow::slotFor(int64_t bucketMs) const {
    const int64_t index = bucketMs / m_timeframeMs;
    const int64_t slot = index % m_width;
    return static_cast<int>(slot < 0 ? slot + m_width : slot);
}

int64_t ColumnWindow::align(int64_t ms) const {
    const int64_t rem = ms % m_timeframeMs;
    return ms - (rem < 0 ? rem + m_timeframeMs : rem);
}

int64_t ColumnWindow::windowStartMs() const {
    return m_windowEndMs - static_cast<int64_t>(m_width - 1) * m_timeframeMs;
}

int64_t ColumnWindow::latestDataMs() const {
    if (m_recording) return m_latestRecordingMs;
    const int64_t newestCachedMs = m_cache.empty() ? 0 : m_cache.rbegin()->first;
    return std::max(m_latestLiveMs, newestCachedMs);
}

bool ColumnWindow::isKnown(int64_t bucketMs) const {
    if (m_floorMs > 0 && bucketMs < m_floorMs) {
        return true;
    }
    auto it = m_known.upper_bound(bucketMs);
    if (it == m_known.begin()) {
        return false;
    }
    --it;
    return bucketMs <= it->second;
}

void ColumnWindow::addKnown(int64_t startMs, int64_t endMs) {
    if (endMs < startMs) {
        return;
    }
    auto it = m_known.upper_bound(startMs);
    if (it != m_known.begin()) {
        auto prev = std::prev(it);
        if (prev->second + m_timeframeMs >= startMs) {
            startMs = prev->first;
            endMs = std::max(endMs, prev->second);
            it = m_known.erase(prev);
        }
    }
    while (it != m_known.end() && it->first <= endMs + m_timeframeMs) {
        endMs = std::max(endMs, it->second);
        it = m_known.erase(it);
    }
    m_known.emplace(startMs, endMs);
}

bool ColumnWindow::acceptShape(const Column& column, int bytesPerCell) {
    if (m_width <= 0 || column.bucketStartMs <= 0 ||
        column.bucketStartMs % m_timeframeMs != 0 ||
        (bytesPerCell != 1 && bytesPerCell != 2) ||
        column.intensity.isEmpty() || column.intensity.size() % bytesPerCell != 0 ||
        !std::isfinite(column.minPrice) || !std::isfinite(column.maxPrice) ||
        !std::isfinite(column.tickSize) || column.maxPrice <= column.minPrice ||
        column.tickSize <= 0.0) {
        return false;
    }
    const int rows = column.intensity.size() / bytesPerCell;
    if (m_rows == 0) {
        m_rows = rows;
        m_bytesPerCell = bytesPerCell;
        m_zeroIntensity = QByteArray(rows * bytesPerCell, 0);
        return true;
    }
    return rows == m_rows && bytesPerCell == m_bytesPerCell;
}

Band ColumnWindow::unionBand(int64_t startMs, int64_t endMs) const {
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    for (auto it = m_cache.lower_bound(startMs); it != m_cache.end() && it->first <= endMs; ++it) {
        lo = std::min(lo, it->second.minPrice);
        hi = std::max(hi, it->second.maxPrice);
    }
    if (hi <= lo || m_rows <= 0) {
        return {};
    }
    return {lo, hi, (hi - lo) / static_cast<double>(m_rows)};
}

const Column* ColumnWindow::newestCached() const {
    return m_cache.empty() ? nullptr : &m_cache.rbegin()->second;
}

bool ColumnWindow::ingestLive(const Column& column, int bytesPerCell, Update& out, bool& firstPlacement) {
    firstPlacement = false;
    if (m_recording) {
        // Source data remains cached even while a recording projection is active.
        // It cannot change the displayed encoding, band or known recording range.
        if (column.bucketStartMs > 0 && column.bucketStartMs % m_timeframeMs == 0 &&
            (bytesPerCell == 1 || bytesPerCell == 2) && !column.intensity.isEmpty()) {
            m_cache[column.bucketStartMs] = column;
            m_latestLiveMs = std::max(m_latestLiveMs, column.bucketStartMs);
            evict();
        }
        return false;
    }
    if (!acceptShape(column, bytesPerCell)) {
        return false;
    }
    const int64_t bucket = column.bucketStartMs;
    m_cache[bucket] = column;
    addKnown(bucket, bucket);
    m_latestLiveMs = std::max(m_latestLiveMs, bucket);
    const bool newest = bucket == m_latestLiveMs;
    evict();

    if (!m_placed) {
        firstPlacement = place(out, {bucket});
        if (firstPlacement && newest) {
            out.liveBucketMs = bucket;
        }
        return firstPlacement;
    }

    if (m_pinned && bucket > m_windowEndMs) {
        // Slide right unless a manual view would fall off the window's left edge.
        const int64_t newStart = bucket - static_cast<int64_t>(m_width - 1) * m_timeframeMs;
        const bool holdView = m_hasView && !m_follow && m_viewStartMs < newStart;
        if (holdView) {
            m_pinned = false;  // window stays; live keeps landing in the cache
            return false;
        }
        m_windowEndMs = bucket;
        const Band liveBand = bandOf(column);
        const bool bandChanged = !liveBand.sameAs(m_band);
        if (bandChanged) {
            m_band = liveBand;
        }
        emitWindow(bandChanged, {bucket}, out);
        out.liveBucketMs = newest ? bucket : 0;
        return true;
    }

    if (bucket >= windowStartMs() && bucket <= m_windowEndMs) {
        const Band liveBand = bandOf(column);
        const bool bandChanged = m_pinned && newest && !liveBand.sameAs(m_band);
        if (bandChanged) {
            m_band = liveBand;
        }
        emitWindow(bandChanged, {bucket}, out);
        out.liveBucketMs = newest ? bucket : 0;
        return true;
    }
    return false;
}

bool ColumnWindow::ingestHistory(const std::vector<Column>& columns,
                                 int bytesPerCell,
                                 int64_t requestEndMs,
                                 int requestedCount,
                                 int64_t oldestAvailableMs,
                                 Update& out,
                                 bool& firstPlacement) {
    firstPlacement = false;
    if (oldestAvailableMs > 0) {
        m_floorMs = oldestAvailableMs;
    }

    std::vector<int64_t> changed;
    changed.reserve(columns.size());
    int64_t oldest = std::numeric_limits<int64_t>::max();
    int64_t newest = kNoBucket;
    for (const auto& column : columns) {
        if (!acceptShape(column, bytesPerCell)) {
            continue;
        }
        // The forming live bucket is newer than anything persisted; keep it.
        if (column.bucketStartMs == m_latestLiveMs && m_cache.count(column.bucketStartMs)) {
            continue;
        }
        m_cache[column.bucketStartMs] = column;
        changed.push_back(column.bucketStartMs);
        oldest = std::min(oldest, column.bucketStartMs);
        newest = std::max(newest, column.bucketStartMs);
    }

    // The server scans back from the request end: a short page means nothing
    // older exists; a full page covers [oldest returned, end].
    const int accepted = static_cast<int>(changed.size());
    const int64_t endBound = requestEndMs > 0 ? align(requestEndMs) : (accepted > 0 ? newest : 0);
    if (endBound > 0) {
        if (accepted < std::max(1, requestedCount)) {
            addKnown(kFarPast, endBound);
        } else {
            addKnown(oldest, endBound);
        }
    }
    evict();

    if (!m_placed) {
        firstPlacement = place(out, changed);
        return firstPlacement;
    }
    emitWindow(false, changed, out);
    return !out.writes.empty();
}

bool ColumnWindow::setDisplayBand(const Band& band, uint64_t generation, Update& out) {
    if (!band.valid() || !std::isfinite(band.minPrice) || !std::isfinite(band.maxPrice) ||
        !std::isfinite(band.tickSize) || (m_recording && generation < m_bandGeneration)) return false;
    const double rowCount = (band.maxPrice - band.minPrice) / band.tickSize;
    if (!std::isfinite(rowCount) || rowCount < 1 || rowCount > 16384 ||
        std::abs(rowCount - std::round(rowCount)) > 1e-6) return false;
    if (m_recording && generation == m_bandGeneration && band.sameAs(m_band)) return false;
    m_recording = true;
    m_bandGeneration = generation;
    m_band = band;
    m_rows = static_cast<int>(std::llround(rowCount));
    m_bytesPerCell = 2;
    m_zeroIntensity = QByteArray(m_rows * 2, 0);
    m_zeroValidity = QByteArray((m_rows + 7) / 8, 0);
    m_projected.clear();
    m_known.clear();
    m_floorMs = 0;
    // Preserve the time anchor even before the first recording page replaces it.
    if (m_latestRecordingMs == 0 && m_placed) m_latestRecordingMs = m_windowEndMs;
    if (!m_placed) return false;
    emitWindow(true, {}, out);
    return true;
}

bool ColumnWindow::ingestRecording(const std::vector<Column>& columns, uint64_t generation,
                                   const std::string& requestId, int64_t scannedStartMs,
                                   int64_t scannedEndMs, bool exhausted, int64_t oldestAvailableMs,
                                   int64_t latestAvailableMs, double sizeFloor, double codesPerOctave,
                                   Update& out, bool& firstPlacement) {
    firstPlacement = false;
    if (!m_recording || generation != m_bandGeneration || requestId != m_requestId ||
        !(sizeFloor > 0) || !std::isfinite(sizeFloor) || !(codesPerOctave > 0) ||
        !std::isfinite(codesPerOctave)) return false;
    m_sizeFloor = sizeFloor;
    m_codesPerOctave = codesPerOctave;
    const auto previousLatest = m_latestRecordingMs;
    if (latestAvailableMs > 0) m_latestRecordingMs = latestAvailableMs;
    std::vector<int64_t> changed;
    changed.reserve(columns.size());
    for (const auto& column : columns) {
        if (!bandOf(column).sameAs(m_band) || !acceptShape(column, 2) ||
            column.validity.size() != (m_rows + 7) / 8 || column.liquidity.size() != m_rows * 2)
            continue;
        m_projected[column.bucketStartMs] = column;
        m_latestRecordingMs = std::max(m_latestRecordingMs, column.bucketStartMs);
        changed.push_back(column.bucketStartMs);
    }
    // Only completed, tf-aligned output buckets are proven by a recording page.
    if (scannedEndMs > scannedStartMs && scannedStartMs > 0 &&
        scannedStartMs % m_timeframeMs == 0 && scannedEndMs % m_timeframeMs == 0)
        addKnown(scannedStartMs, scannedEndMs - m_timeframeMs);
    if (exhausted) {
        if (oldestAvailableMs > 0) m_floorMs = oldestAvailableMs;
        else if (scannedStartMs > 0) m_floorMs = scannedStartMs;
    }
    // Projection cache is disposable; source/live cache is never touched by a re-band.
    while (static_cast<int>(m_projected.size()) > m_capacity) {
        const auto front = m_projected.begin();
        const auto back = std::prev(m_projected.end());
        m_projected.erase(m_placed && front->first >= windowStartMs() ? back : front);
    }
    if (!m_placed) {
        firstPlacement = place(out, changed);
        return firstPlacement;
    }
    if (m_latestRecordingMs > previousLatest) return place(out, changed);
    emitWindow(false, changed, out);
    return !out.writes.empty();
}

bool ColumnWindow::setViewport(int64_t viewStartMs, int64_t viewEndMs, bool follow, Update& out) {
    m_hasView = viewEndMs > viewStartMs;
    m_viewStartMs = viewStartMs;
    m_viewEndMs = viewEndMs;
    m_follow = follow;
    if (latestDataMs() <= 0) {
        return false;
    }
    return place(out, {});
}

bool ColumnWindow::place(Update& out, const std::vector<int64_t>& changed) {
    const int64_t latest = latestDataMs();
    if (latest <= 0 || m_rows <= 0) {
        return false;
    }
    const int64_t tf = m_timeframeMs;
    const int64_t width = m_width;

    int64_t end = latest;
    bool pin = true;
    if (m_hasView && !m_follow && m_viewEndMs < latest) {
        const int64_t viewStart = align(m_viewStartMs);
        const int64_t viewEnd = align(m_viewEndMs);
        const int64_t viewBuckets = std::max<int64_t>(1, (viewEnd - viewStart) / tf + 1);
        const int64_t slack = std::max<int64_t>(0, width - viewBuckets);
        const int64_t margin = slack / 8;
        const bool inside = m_placed &&
            viewStart >= windowStartMs() + margin * tf &&
            viewEnd <= m_windowEndMs - margin * tf;
        if (inside) {
            end = m_windowEndMs;
            pin = m_pinned && end == latest;
        } else {
            end = std::min(viewEnd + (slack / 2) * tf, latest);
            pin = end == latest;
        }
    }

    const bool jump = !m_placed || std::llabs(end - m_windowEndMs) >= width * tf;
    Band band = m_band;
    if (!m_recording && pin && newestCached()) {
        band = bandOf(*newestCached());
    } else if (!m_recording && (jump || !band.valid())) {
        band = unionBand(end - (width - 1) * tf, end);
        if (!band.valid()) {
            band = m_band.valid() ? m_band : (newestCached() ? bandOf(*newestCached()) : Band{});
        }
    }
    if (!band.valid()) {
        return false;
    }

    const bool full = jump || !band.sameAs(m_band);
    if (!full && end == m_windowEndMs && changed.empty()) {
        m_pinned = pin;
        return false;
    }
    m_placed = true;
    m_pinned = pin;
    m_windowEndMs = end;
    m_band = band;
    emitWindow(full, changed, out);
    return true;
}

void ColumnWindow::emitWindow(bool full, const std::vector<int64_t>& changed, Update& out) {
    out = Update{};
    out.timeframeMs = m_timeframeMs;
    out.width = m_width;
    out.rows = m_rows;
    out.bytesPerCell = m_bytesPerCell;
    out.band = m_band;
    out.valueEncoding = m_recording ? ValueEncoding::AbsoluteLogSize : ValueEncoding::LegacyIntensity;
    out.bandGeneration = m_bandGeneration;
    out.sizeFloor = m_sizeFloor;
    out.codesPerOctave = m_codesPerOctave;
    out.windowEndMs = m_windowEndMs;
    out.newestSlot = slotFor(m_windowEndMs);
    out.full = full;
    out.pinnedToLive = m_pinned;

    const int64_t start = windowStartMs();
    if (full) {
        out.writes.resize(static_cast<size_t>(m_width));
        for (int i = 0; i < m_width; ++i) {
            writeSlot(start + static_cast<int64_t>(i) * m_timeframeMs, out.writes[static_cast<size_t>(i)]);
        }
    } else {
        for (int i = 0; i < m_width; ++i) {
            const int64_t bucket = start + static_cast<int64_t>(i) * m_timeframeMs;
            if (m_slotBucket[static_cast<size_t>(slotFor(bucket))] != bucket) {
                out.writes.emplace_back();
                writeSlot(bucket, out.writes.back());
            }
        }
        for (const int64_t bucket : changed) {
            if (bucket < start || bucket > m_windowEndMs) {
                continue;
            }
            const int slot = slotFor(bucket);
            const bool alreadyWritten = std::any_of(out.writes.begin(), out.writes.end(),
                [slot](const SlotWrite& write) { return write.slot == slot; });
            if (!alreadyWritten) {
                out.writes.emplace_back();
                writeSlot(bucket, out.writes.back());
            }
        }
    }

    out.coverage = QByteArray(m_width, 0);
    char* coverage = out.coverage.data();
    for (int i = 0; i < m_width; ++i) {
        const int64_t bucket = start + static_cast<int64_t>(i) * m_timeframeMs;
        const auto slot = static_cast<size_t>(slotFor(bucket));
        coverage[i] = (m_slotBucket[slot] == bucket && m_slotRecorded[slot]) ? 1 : 0;
    }
}

void ColumnWindow::writeSlot(int64_t bucketMs, SlotWrite& out) {
    const int slot = slotFor(bucketMs);
    out.slot = slot;
    out.bucketStartMs = bucketMs;
    const auto& cache = m_recording ? m_projected : m_cache;
    const auto it = cache.find(bucketMs);
    if (it != cache.end()) {
        if (m_recording) {
            out.intensity = it->second.intensity;
            out.liquidity = it->second.liquidity;
            out.validity = it->second.validity;
        } else resampleColumn(it->second, m_band, m_rows, m_bytesPerCell, out.intensity, out.liquidity);
        out.liquidityScale = it->second.liquidityScale > 0.0 ? it->second.liquidityScale : 1.0;
        out.recorded = true;
    } else {
        out.intensity = m_zeroIntensity;
        out.liquidity.clear();
        out.liquidityScale = 1.0;
        out.recorded = false;
        if (m_recording) out.validity = m_zeroValidity;
    }
    m_slotBucket[static_cast<size_t>(slot)] = bucketMs;
    m_slotRecorded[static_cast<size_t>(slot)] = out.recorded ? 1 : 0;
}

void ColumnWindow::evict() {
    while (static_cast<int>(m_cache.size()) > m_capacity) {
        const int64_t start = m_placed ? windowStartMs() : std::numeric_limits<int64_t>::max();
        const int64_t end = m_placed ? m_windowEndMs : std::numeric_limits<int64_t>::min();
        const int64_t centre = m_placed ? end - static_cast<int64_t>(m_width / 2) * m_timeframeMs
                                        : latestDataMs();
        auto front = m_cache.begin();
        auto back = std::prev(m_cache.end());
        const bool frontInWindow = front->first >= start && front->first <= end;
        const bool backInWindow = back->first >= start && back->first <= end;
        if (frontInWindow && backInWindow) {
            break;
        }
        const bool evictFront = backInWindow ||
            (!frontInWindow && (centre - front->first) >= (back->first - centre));
        m_cache.erase(evictFront ? front : back);
    }
}

bool ColumnWindow::nextFetch(FetchRequest& out) const {
    if (!m_placed) {
        return false;
    }
    const int64_t tf = m_timeframeMs;
    const int64_t windowStart = windowStartMs();
    int64_t hi = m_windowEndMs;
    int64_t lo = m_windowEndMs - static_cast<int64_t>(kPageColumns - 1) * tf;
    if (m_hasView) {
        hi = std::min(hi, align(m_viewEndMs));
        lo = align(m_viewStartMs) - static_cast<int64_t>(kPageColumns) * tf;  // one page of prefetch
    }
    lo = std::max(lo, windowStart);
    for (int64_t bucket = hi; bucket >= lo; bucket -= tf) {
        if (!(m_recording ? m_projected : m_cache).count(bucket) && !isKnown(bucket)) {
            out.endMs = bucket;
            out.count = kPageColumns;
            return true;
        }
    }
    return false;
}

} // namespace heatmap_window
