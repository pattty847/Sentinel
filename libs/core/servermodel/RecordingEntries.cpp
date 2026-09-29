#include "RecordingEntries.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace recording {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point then) {
    return std::chrono::duration<double, std::milli>(Clock::now() - then).count();
}
constexpr int64_t minute = 60'000;
struct Entry { uint32_t col; int64_t row; uint32_t side; uint16_t code; uint32_t coveredMs; };
struct RawRun { uint32_t col; int64_t lo, hi; uint32_t side, coveredMs; };
void finish(RecordingEntries &out, std::vector<Entry> &entries,
            std::vector<std::array<int64_t, 4>> &bounds,
            const std::vector<int64_t> &nativeUnits,
            const std::vector<SizeScale> &columnScales,
            std::vector<RawRun> &runs, int64_t commonUnits) {
    for (auto &e : entries) e.row = floorDiv(e.row, commonUnits);
    for (size_t c = 0; c < bounds.size(); ++c) {
        if (!nativeUnits[c]) continue;
        auto &b = bounds[c];
        for (int i : {0, 2}) {
            if (b[i] > b[i + 1]) continue;
            // A common row is valid only if every underlying native row was
            // covered, including the zero-valued rows absent from entries.
            const int64_t loUnits = b[i] * nativeUnits[c];
            const int64_t endUnits = (b[i + 1] + 1) * nativeUnits[c];
            b[i] = -floorDiv(-loUnits, commonUnits);
            b[i + 1] = floorDiv(endUnits, commonUnits) - 1;
        }
    }
    int64_t base = std::numeric_limits<int64_t>::max();
    for (const auto &e : entries) base = std::min(base, e.row);
    for (const auto &b : bounds)
        for (int i : {0, 2}) if (b[i] <= b[i + 1]) base = std::min(base, b[i]);
    for (const auto &run : runs) base = std::min(base, floorDiv(run.lo, commonUnits));
    out.baseRow = base == std::numeric_limits<int64_t>::max() ? 0 : base;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.col != b.col) return a.col < b.col;
        if (a.row != b.row) return a.row < b.row;
        return a.side < b.side;
    });
    out.rowSide.reserve(entries.size()); out.code.reserve(entries.size());
    out.offsets.resize(out.columns() + 1);
    out.nativeFactor.resize(out.columns());
    out.columnScale = columnScales;
    size_t next = 0;
    for (uint32_t c = 0; c < out.columns(); ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.rowSide.size());
        while (next < entries.size() && entries[next].col == c) {
            const auto &e = entries[next++];
            const auto rel = e.row - out.baseRow;
            if (rel < 0 || rel > 0x7fffffffu)
                throw std::runtime_error("recording native row range exceeds GPU index width");
            const uint32_t key = uint32_t(rel) | (e.side << 31);
            // Retain each native entry. Re-encoding a partial sum here loses
            // precision and can saturate the HMC2 15-bit range.
            out.rowSide.push_back(key);
            out.code.push_back(e.code);
            if (out.sourceMinutes == 60) out.entryCoveredMs.push_back(e.coveredMs);
        }
        if (nativeUnits[c] / commonUnits > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("recording native tick factor exceeds GPU width");
        out.nativeFactor[c] = nativeUnits[c] ? uint32_t(nativeUnits[c] / commonUnits) : 0;
        const auto &b = bounds[c];
        auto convert = [&](int i) -> std::pair<int32_t, int32_t> {
            if (b[i] > b[i + 1]) return {1, 0};
            const auto lo = b[i] - out.baseRow, hi = b[i + 1] - out.baseRow;
            if (lo < 0 || hi > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("recording coverage exceeds GPU index width");
            return {static_cast<int32_t>(lo), static_cast<int32_t>(hi)};
        };
        const auto bid = convert(0), ask = convert(2);
        out.coverage[c] = {bid.first, bid.second, ask.first, ask.second};
    }
    out.offsets.back() = static_cast<uint32_t>(out.rowSide.size());
    if (out.sourceMinutes == 60) {
        std::sort(runs.begin(), runs.end(), [](const RawRun &a, const RawRun &b) {
            if (a.col != b.col) return a.col < b.col;
            if (a.side != b.side) return a.side < b.side;
            return a.lo < b.lo;
        });
        out.coverageRunOffsets.resize(size_t(out.columns()) * 2 + 1);
        size_t nextRun = 0;
        for (uint32_t c = 0; c < out.columns(); ++c) for (uint32_t side = 0; side < 2; ++side) {
            out.coverageRunOffsets[c * 2 + side] = uint32_t(out.coverageRuns.size());
            while (nextRun < runs.size() && runs[nextRun].col == c && runs[nextRun].side == side) {
                const auto &run = runs[nextRun++];
                const auto lo = -floorDiv(-run.lo, commonUnits) - out.baseRow;
                const auto hi = floorDiv(run.hi + 1, commonUnits) - 1 - out.baseRow;
                if (lo < 0 || hi > std::numeric_limits<int32_t>::max())
                    throw std::runtime_error("recording hour coverage exceeds GPU row width");
                if (lo <= hi)
                    out.coverageRuns.push_back({int32_t(lo), int32_t(hi), int32_t(run.coveredMs), int32_t(side)});
            }
        }
        out.coverageRunOffsets.back() = uint32_t(out.coverageRuns.size());
    }
}

struct ScanChunk {
    std::vector<Entry> entries;
    std::vector<std::array<int64_t, 4>> bounds;
    std::vector<int64_t> nativeUnits;
    std::vector<uint32_t> observedMs;
    std::vector<SizeScale> columnScale;
    std::vector<RawRun> runs;
    double priceScale = 0, decodeMs = 0;
    SizeScale sizeScale;
    int64_t commonUnits = 0;
};

ScanChunk scanChunk(const std::filesystem::path &root, const std::string &symbol,
                    const std::string &layer, int64_t rangeStart, int64_t chunkStart,
                    int64_t chunkEnd, uint32_t columns, uint32_t sourceMinutes) {
    ScanChunk chunk;
    chunk.bounds.resize(columns, {1, 0, 1, 0});
    chunk.nativeUnits.resize(columns);
    chunk.observedMs.resize(columns);
    chunk.columnScale.resize(columns);
    std::string failure;
    Hmc2Reader reader(root); // reader/index/cache belongs to this worker
    ReadControl control;
    const int64_t sourceMs = int64_t(sourceMinutes) * minute;
    const auto scan = reader.visit(symbol, layer, sourceMs, chunkStart, chunkEnd, [&](const Hmc2Record &r) {
        if (!failure.empty()) return;
        const auto decoded = Clock::now();
        const double tick = r.header.rowTickUnits / r.header.priceScale;
        if (!std::isfinite(tick) || tick <= 0 || !std::isfinite(r.header.priceScale) ||
            r.header.priceScale <= 0) { failure = "bad recording native grid"; return; }
        if (!chunk.commonUnits) chunk.priceScale = r.header.priceScale;
        else if (r.header.priceScale != chunk.priceScale) { failure = "mixed price scales in recording range"; return; }
        chunk.sizeScale = r.header.sizeScale;
        const int64_t units = r.header.rowTickUnits;
        chunk.commonUnits = chunk.commonUnits ? std::gcd(chunk.commonUnits, units) : units;
        const auto col = static_cast<uint32_t>((r.bucketStartMs - rangeStart) / sourceMs);
        if (col >= columns || !r.observedMs) return;
        chunk.bounds[col] = {r.bidRowLo, r.bidRowHi, r.askRowLo, r.askRowHi};
        chunk.nativeUnits[col] = units;
        chunk.observedMs[col] = r.observedMs;
        chunk.columnScale[col] = r.header.sizeScale;
        if (sourceMinutes == 60) {
            if (!(r.flags & kApproximateCoverage) && !r.coverage.empty()) {
                for (const auto &run : r.coverage)
                    if (run.lo <= run.hi && run.coveredMs)
                        chunk.runs.push_back({col, run.lo * units, (run.hi + 1) * units - 1,
                                              uint32_t(run.isAsk), run.coveredMs});
            } else {
                for (uint32_t side = 0; side < 2; ++side) {
                    const auto lo = side ? r.askRowLo : r.bidRowLo;
                    const auto hi = side ? r.askRowHi : r.bidRowHi;
                    if (lo <= hi) chunk.runs.push_back({col, lo * units, (hi + 1) * units - 1,
                                                        side, r.observedMs});
                }
            }
        }
        for (const auto &e : r.entries) {
            const auto lo = e.isAsk ? r.askRowLo : r.bidRowLo;
            const auto hi = e.isAsk ? r.askRowHi : r.bidRowHi;
            if (e.row < lo || e.row > hi) continue;
            const double decodedSize = decodeSize(e.twapCode, r.header.sizeScale);
            if (!std::isfinite(decodedSize) || decodedSize < 0 ||
                decodedSize > std::numeric_limits<float>::max()) {
                failure = "nonfinite recording size"; return;
            }
            if (decodedSize > 0) {
                if (e.row > std::numeric_limits<int64_t>::max() / units ||
                    e.row < std::numeric_limits<int64_t>::min() / units) {
                    failure = "native price unit overflow"; return;
                }
                chunk.entries.push_back({col, e.row * units, uint32_t(e.isAsk),
                                         uint16_t(e.twapCode & kMaxCode),
                                         sourceMinutes == 60 ? e.coveredMs : r.observedMs});
            }
        }
        chunk.decodeMs += elapsed(decoded);
    }, control);
    if (!failure.empty()) throw std::runtime_error(failure);
    if (scan.status != ReadStatus::Complete)
        throw std::runtime_error("recording reader did not complete range scan");
    return chunk;
}
} // namespace

RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs, uint32_t sourceMinutes) {
    const int64_t sourceMs = int64_t(sourceMinutes) * minute;
    if ((layer != "near" && layer != "deep") || symbol.empty() || startMs >= endMs ||
        (sourceMinutes != 1 && sourceMinutes != 60) ||
        startMs < kHmc2MinMs || endMs > kHmc2EndMs || startMs % sourceMs || endMs % sourceMs ||
        (endMs - startMs) / sourceMs > 100'000)
        throw std::invalid_argument("invalid recording entry range");
    const auto started = Clock::now();
    RecordingEntries out;
    out.startMs = startMs;
    out.sourceMinutes = sourceMinutes;
    out.coverage.resize(static_cast<size_t>((endMs - startMs) / sourceMs));
    out.observedMs.resize(out.columns());
    std::vector<std::array<int64_t, 4>> bounds(out.columns(), {1, 0, 1, 0});
    std::vector<int64_t> nativeUnits(out.columns(), 0);
    std::vector<SizeScale> columnScales(out.columns());
    std::vector<Entry> entries;
    std::vector<RawRun> runs;
    int64_t commonUnits = 0;
    const uint32_t totalColumns = out.columns();
    const uint32_t workers = std::min<uint32_t>(4, std::max<uint32_t>(1, totalColumns / (360 / sourceMinutes)));
    const uint32_t chunkColumns = (totalColumns + workers - 1) / workers;
    std::vector<std::future<ScanChunk>> tasks;
    tasks.reserve(workers);
    for (uint32_t worker = 0; worker < workers; ++worker) {
        const auto from = startMs + int64_t(worker * chunkColumns) * sourceMs;
        const auto to = std::min(endMs, from + int64_t(chunkColumns) * sourceMs);
        if (from >= to) break;
        tasks.push_back(std::async(std::launch::async, [&, from, to] {
            return scanChunk(root, symbol, layer, startMs, from, to, totalColumns, sourceMinutes);
        }));
    }
    bool selectedScale = false;
    for (auto &task : tasks) {
        auto chunk = task.get();
        if (chunk.commonUnits) {
            if (!selectedScale) { out.priceScale = chunk.priceScale; selectedScale = true; }
            else if (out.priceScale != chunk.priceScale) throw std::runtime_error("mixed price scales in recording range");
            out.sizeScale = chunk.sizeScale;
            commonUnits = commonUnits ? std::gcd(commonUnits, chunk.commonUnits) : chunk.commonUnits;
        }
        out.decodeMs += chunk.decodeMs;
        for (uint32_t c = 0; c < totalColumns; ++c) if (chunk.nativeUnits[c]) {
            bounds[c] = chunk.bounds[c]; nativeUnits[c] = chunk.nativeUnits[c];
            out.observedMs[c] = chunk.observedMs[c];
            columnScales[c] = chunk.columnScale[c];
        }
        entries.insert(entries.end(), std::make_move_iterator(chunk.entries.begin()),
                       std::make_move_iterator(chunk.entries.end()));
        runs.insert(runs.end(), std::make_move_iterator(chunk.runs.begin()),
                    std::make_move_iterator(chunk.runs.end()));
    }
    if (commonUnits) out.nativeTick = commonUnits / out.priceScale;
    finish(out, entries, bounds, nativeUnits, columnScales, runs, commonUnits ? commonUnits : 1);
    out.loadMs = elapsed(started);
    return out;
}


namespace {
// Compose a selected UTC timeframe once. Each native grid keeps its own
// per-row/side covered duration; only then are grid values weighted together.
std::optional<RecordingEntries> composeBucketFromSource(const RecordingEntries &source,
                                                          int64_t bucketStartMs,
                                                          uint32_t durationMinutes,
                                                          bool preNormalize) {
    if (source.preNormalized || bucketStartMs < source.startMs ||
        (bucketStartMs - source.startMs) % (int64_t(source.sourceMinutes) * minute) ||
        durationMinutes < 2 || durationMinutes > 1440 ||
        durationMinutes % source.sourceMinutes) return std::nullopt;
    const uint32_t first = uint32_t((bucketStartMs - source.startMs) /
                                    (int64_t(source.sourceMinutes) * minute));
    const uint32_t count = durationMinutes / source.sourceMinutes;
    if (first + count > source.columns()) return std::nullopt;
    struct NativeGroup {
        uint64_t observedMs = 0;
        std::array<std::map<int32_t, int64_t>, 2> edges;
        std::unordered_map<uint64_t, long double> numerators;
    };
    RecordingEntries out;
    out.startMs = bucketStartMs;
    out.sourceMinutes = durationMinutes;
    out.preNormalized = preNormalize;
    out.baseRow = source.baseRow;
    out.nativeTick = source.nativeTick;
    out.priceScale = source.priceScale;
    out.sizeScale = source.sizeScale;
    out.coverage.resize(1);
    out.observedMs.resize(1);
    out.nativeFactor.resize(1);
    out.columnScale.resize(1);
    out.coverageRunOffsets.resize(3);
    std::map<uint32_t, NativeGroup> grids;
    std::array<std::map<int32_t, int64_t>, 2> totalEdges;
    uint32_t singleFactor = 0;
    for (uint32_t c = first; c < first + count; ++c) {
        if (!source.observedMs[c]) continue;
        const uint32_t factor = source.nativeFactor[c];
        if (!factor) return std::nullopt;
        if (!singleFactor) singleFactor = factor;
        else if (!preNormalize && factor != singleFactor) return std::nullopt;
        auto &grid = grids[factor];
        grid.observedMs += source.observedMs[c];
        out.observedMs[0] += source.observedMs[c];
        out.sizeScale = source.columnScale[c];
        out.columnScale[0] = out.sizeScale;
        for (uint32_t side = 0; side < 2; ++side) {
            auto addCoverage = [&](int32_t lo, int32_t hi, uint32_t coveredMs) {
                if (lo > hi || !coveredMs) return;
                grid.edges[side][lo] += coveredMs;
                grid.edges[side][hi + 1] -= coveredMs;
                totalEdges[side][lo] += coveredMs;
                totalEdges[side][hi + 1] -= coveredMs;
            };
            if (source.sourceMinutes == 1) {
                const auto &cov = source.coverage[c];
                addCoverage(side ? cov.askLo : cov.bidLo, side ? cov.askHi : cov.bidHi,
                            source.observedMs[c]);
            } else {
                for (uint32_t j = source.coverageRunOffsets[c * 2 + side];
                     j < source.coverageRunOffsets[c * 2 + side + 1]; ++j) {
                    const auto &run = source.coverageRuns[j];
                    addCoverage(run[0], run[1], uint32_t(run[2]));
                }
            }
        }
        for (uint32_t j = source.offsets[c]; j < source.offsets[c + 1]; ++j) {
            const uint64_t key = (uint64_t(source.rowSide[j] & 0x7fffffffu) << 1) |
                                 (source.rowSide[j] >> 31);
            const uint32_t numeratorMs = source.sourceMinutes == 1 ?
                source.observedMs[c] : source.entryCoveredMs[j];
            grid.numerators[key] += decodeSize(uint16_t(source.code[j]), source.columnScale[c]) *
                                    numeratorMs;
        }
    }
    uint64_t factorLcm = 0;
    for (auto &[factor, grid] : grids) {
        factorLcm = factorLcm ? std::lcm(factorLcm, uint64_t(factor)) : factor;
        if (factorLcm > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("composed native-grid factor exceeds GPU width");
        for (auto &side : grid.edges) {
            int64_t covered = 0;
            for (auto &[edge, delta] : side) {
                covered += delta;
                delta = covered;
            }
        }
    }
    out.nativeFactor[0] = uint32_t(factorLcm);
    for (uint32_t side = 0; side < 2; ++side) {
        out.coverageRunOffsets[side] = uint32_t(out.coverageRuns.size());
        int64_t covered = 0;
        for (auto it = totalEdges[side].begin(); it != totalEdges[side].end(); ++it) {
            covered += it->second;
            it->second = covered;
            auto next = std::next(it);
            if (covered > 0 && next != totalEdges[side].end() && next->first > it->first) {
                if (covered > std::numeric_limits<int32_t>::max())
                    throw std::runtime_error("timeframe coverage duration exceeds GPU width");
                out.coverageRuns.push_back({it->first, int32_t(next->first - 1),
                                            int32_t(covered), int32_t(side)});
                auto &cov = out.coverage[0];
                int32_t &lo = side ? cov.askLo : cov.bidLo;
                int32_t &hi = side ? cov.askHi : cov.bidHi;
                if (lo > hi) { lo = it->first; hi = int32_t(next->first - 1); }
                else { lo = std::min(lo, it->first); hi = std::max(hi, int32_t(next->first - 1)); }
            }
        }
    }
    out.coverageRunOffsets[2] = uint32_t(out.coverageRuns.size());
    std::unordered_map<uint64_t, long double> values;
    for (const auto &[factor, grid] : grids) {
        const long double gridWeight = preNormalize ?
            static_cast<long double>(grid.observedMs) / out.observedMs[0] : 1.0L;
        for (const auto &[key, numerator] : grid.numerators) {
            const auto side = uint32_t(key & 1u);
            const auto row = int32_t(key >> 1);
            const auto &edge = grid.edges[side];
            auto it = edge.upper_bound(row);
            if (it == edge.begin()) continue;
            const int64_t covered = std::prev(it)->second;
            if (covered > 0) values[key] += numerator / covered * gridWeight;
        }
    }
    out.offsets.push_back(0);
    std::vector<uint64_t> keys;
    keys.reserve(values.size());
    for (const auto &item : values) keys.push_back(item.first);
    std::sort(keys.begin(), keys.end());
    for (const auto key : keys) {
        const uint32_t row = uint32_t(key >> 1), side = uint32_t(key & 1u);
        const double quantity = double(values.at(key));
        if (!std::isfinite(quantity)) throw std::runtime_error("nonfinite composed quantity");
        const auto &edge = totalEdges[side];
        auto it = edge.upper_bound(int32_t(row));
        const uint32_t covered = it == edge.begin() ? 0u : uint32_t(std::prev(it)->second);
        if (!covered) continue;
        out.rowSide.push_back(row | (side << 31));
        out.code.push_back(encodeSize(quantity, out.sizeScale));
        out.entryCoveredMs.push_back(preNormalize ? out.observedMs[0] : covered);
    }
    out.offsets.push_back(uint32_t(out.rowSide.size()));
    return out;
}
} // namespace

namespace {
RecordingEntries composeRange(const RecordingEntries &source, int64_t startMs, int64_t endMs,
                              uint32_t timeframeMinutes, Clock::time_point started) {
    const int64_t timeframeMs = int64_t(timeframeMinutes) * minute;
    const uint32_t columns = uint32_t((endMs - startMs) / timeframeMs);
    RecordingEntries out;
    out.startMs = startMs;
    out.sourceMinutes = timeframeMinutes;
    out.preNormalized = true;
    out.baseRow = source.baseRow;
    out.nativeTick = source.nativeTick;
    out.priceScale = source.priceScale;
    out.sizeScale = source.sizeScale;
    out.coverage.reserve(columns);
    out.observedMs.reserve(columns);
    out.nativeFactor.reserve(columns);
    out.columnScale.reserve(columns);
    out.offsets.resize(columns + 1);
    out.coverageRunOffsets.resize(size_t(columns) * 2 + 1);
    for (uint32_t c = 0; c < columns; ++c) {
        const auto bucket = composeBucketFromSource(source, startMs + int64_t(c) * timeframeMs,
                                                     timeframeMinutes, true);
        if (!bucket) return source;
        out.offsets[c] = uint32_t(out.rowSide.size());
        out.rowSide.insert(out.rowSide.end(), bucket->rowSide.begin(), bucket->rowSide.end());
        out.code.insert(out.code.end(), bucket->code.begin(), bucket->code.end());
        out.entryCoveredMs.insert(out.entryCoveredMs.end(),
                                  bucket->entryCoveredMs.begin(), bucket->entryCoveredMs.end());
        out.coverage.push_back(bucket->coverage[0]);
        out.observedMs.push_back(bucket->observedMs[0]);
        out.nativeFactor.push_back(bucket->nativeFactor[0]);
        out.columnScale.push_back(bucket->columnScale[0]);
        if (bucket->observedMs[0]) out.sizeScale = bucket->sizeScale;
        for (uint32_t side = 0; side < 2; ++side) {
            out.coverageRunOffsets[c * 2 + side] = uint32_t(out.coverageRuns.size());
            out.coverageRuns.insert(out.coverageRuns.end(),
                bucket->coverageRuns.begin() + bucket->coverageRunOffsets[side],
                bucket->coverageRuns.begin() + bucket->coverageRunOffsets[side + 1]);
        }
    }
    out.offsets.back() = uint32_t(out.rowSide.size());
    out.coverageRunOffsets.back() = uint32_t(out.coverageRuns.size());
    out.decodeMs = source.decodeMs;
    out.loadMs = elapsed(started);
    return out;
}
} // namespace

RecordingEntries loadComposedMinuteEntries(const std::filesystem::path &root,
                                           const std::string &symbol, const std::string &layer,
                                           int64_t startMs, int64_t endMs, uint32_t timeframeMinutes) {
    if (timeframeMinutes == 1) return loadRecordingEntries(root, symbol, layer, startMs, endMs);
    const int64_t timeframeMs = int64_t(timeframeMinutes) * minute;
    if (timeframeMinutes > 1440 || startMs >= endMs ||
        startMs % timeframeMs || endMs % timeframeMs)
        throw std::invalid_argument("composed minute range must use UTC timeframe boundaries");
    const auto started = Clock::now();
    const auto minutes = loadRecordingEntries(root, symbol, layer, startMs, endMs);
    return composeRange(minutes, startMs, endMs, timeframeMinutes, started);
}

RecordingEntries loadHourEntriesWithMinuteTail(const std::filesystem::path &root,
                                               const std::string &symbol, const std::string &layer,
                                               int64_t startMs, int64_t endMs) {
    if (layer != "deep") return loadRecordingEntries(root, symbol, layer, startMs, endMs);
    auto hours = loadRecordingEntries(root, symbol, layer, startMs, endMs, 60);
    uint32_t tail = hours.columns();
    while (tail > 0 && hours.observedMs[tail - 1] == 0) --tail;
    if (tail == hours.columns()) return hours;
    const int64_t tailStart = startMs + int64_t(tail) * 3'600'000;
    const auto minutes = loadRecordingEntries(root, symbol, layer, tailStart, endMs);
    if (tail == 0 && minutes.rowSide.empty()) return hours;
    RecordingEntries result;
    bool haveResult = false;
    if (tail > 0) {
        result = loadRecordingEntries(root, symbol, layer, startMs, tailStart, 60);
        haveResult = true;
    }
    for (uint32_t c = tail; c < hours.columns(); ++c) {
        const auto composed = composeBucketFromSource(minutes, startMs + int64_t(c) * 3'600'000, 60, false);
        if (!composed) return loadRecordingEntries(root, symbol, layer, startMs, endMs);
        if (!haveResult) { result = *composed; haveResult = true; }
        else result = joinRecordingEntries(result, *composed);
    }
    return result;
}

RecordingEntries loadComposedHourEntries(const std::filesystem::path &root,
                                         const std::string &symbol, const std::string &layer,
                                         int64_t startMs, int64_t endMs, uint32_t timeframeMinutes) {
    const int64_t timeframeMs = int64_t(timeframeMinutes) * minute;
    if (layer != "deep" || timeframeMinutes < 60 || timeframeMinutes > 1440 ||
        timeframeMinutes % 60 || startMs >= endMs ||
        startMs % timeframeMs || endMs % timeframeMs)
        throw std::invalid_argument("composed hour range must use UTC hour timeframes");
    const auto started = Clock::now();
    auto source = loadHourEntriesWithMinuteTail(root, symbol, layer, startMs, endMs);
    if (timeframeMinutes == 60) return source;
    return composeRange(source, startMs, endMs, timeframeMinutes, started);
}

RecordingEntries joinRecordingEntries(const RecordingEntries &older, const RecordingEntries &recent) {
    const auto started = Clock::now();
    if (older.sourceMinutes != recent.sourceMinutes ||
        older.preNormalized != recent.preNormalized ||
        older.startMs + int64_t(older.columns()) * older.sourceMinutes * minute != recent.startMs ||
        older.columns() + uint64_t(recent.columns()) > 100'000 ||
        (older.nativeTick > 0 && recent.nativeTick > 0 && older.priceScale != recent.priceScale))
        throw std::invalid_argument("recording entry ranges are not adjacent compatible layers");
    RecordingEntries out;
    out.startMs = older.startMs;
    out.sourceMinutes = older.sourceMinutes;
    out.preNormalized = older.preNormalized;
    out.priceScale = recent.nativeTick > 0 ? recent.priceScale : older.priceScale;
    out.sizeScale = recent.nativeTick > 0 ? recent.sizeScale : older.sizeScale;
    auto units = [&](const RecordingEntries &source) -> int64_t {
        if (!(source.nativeTick > 0)) return 0;
        const double value = source.nativeTick * out.priceScale;
        if (!std::isfinite(value) || value < 1 || value > double(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("recording common-grid units out of range");
        return std::llround(value);
    };
    const int64_t olderUnits = units(older), recentUnits = units(recent);
    const int64_t commonUnits = olderUnits && recentUnits ? std::gcd(olderUnits, recentUnits) :
                                (olderUnits ? olderUnits : recentUnits);
    if (!commonUnits) throw std::invalid_argument("both recording entry ranges are empty");
    out.nativeTick = commonUnits / out.priceScale;
    const int64_t olderRatio = olderUnits ? olderUnits / commonUnits : 1;
    const int64_t recentRatio = recentUnits ? recentUnits / commonUnits : 1;
    const auto checkedAdd = [](int64_t a, int64_t b) -> int64_t {
        if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
            throw std::runtime_error("joined recording row addition overflows int64");
        return a + b;
    };
    const auto checkedScale = [](int64_t row, int64_t ratio) -> int64_t {
        if (ratio < 1 || row > std::numeric_limits<int64_t>::max() / ratio ||
            row < std::numeric_limits<int64_t>::min() / ratio)
            throw std::runtime_error("joined recording row scaling overflows int64");
        return row * ratio;
    };
    const auto checkedSub = [](int64_t a, int64_t b) -> int64_t {
        if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
            (b < 0 && a > std::numeric_limits<int64_t>::max() + b))
            throw std::runtime_error("joined recording row subtraction overflows int64");
        return a - b;
    };
    const auto scaledRow = [&](int64_t base, int64_t relative, int64_t ratio) {
        return checkedScale(checkedAdd(base, relative), ratio);
    };
    out.baseRow = olderUnits && recentUnits ?
        std::min(checkedScale(older.baseRow, olderRatio),
                 checkedScale(recent.baseRow, recentRatio)) :
        (olderUnits ? checkedScale(older.baseRow, olderRatio) :
                      checkedScale(recent.baseRow, recentRatio));
    const size_t columns = size_t(older.columns()) + recent.columns();
    out.offsets.resize(columns + 1);
    out.coverage.reserve(columns); out.observedMs.reserve(columns);
    out.nativeFactor.reserve(columns); out.columnScale.reserve(columns);
    out.rowSide.reserve(older.rowSide.size() + recent.rowSide.size());
    out.code.reserve(older.code.size() + recent.code.size());
    if (out.sourceMinutes != 1) {
        out.entryCoveredMs.reserve(older.entryCoveredMs.size() + recent.entryCoveredMs.size());
        out.coverageRunOffsets.resize(columns * 2 + 1);
    }
    uint32_t outputColumn = 0;
    auto append = [&](const RecordingEntries &source, int64_t ratio) {
        if (source.offsets.size() != size_t(source.columns()) + 1 ||
            source.code.size() != source.rowSide.size() ||
            source.observedMs.size() != source.columns() ||
            source.nativeFactor.size() != source.columns() ||
            source.columnScale.size() != source.columns())
            throw std::invalid_argument("invalid source recording entry arrays");
        auto project = [&](int32_t lo, int32_t hi) -> std::pair<int32_t, int32_t> {
            if (lo > hi) return {1, 0};
            const auto first = checkedSub(scaledRow(source.baseRow, lo, ratio), out.baseRow);
            const auto last = checkedSub(checkedSub(scaledRow(source.baseRow, int64_t(hi) + 1, ratio),
                                                   out.baseRow), 1);
            if (first < 0 || last > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("joined recording coverage exceeds GPU row width");
            return {int32_t(first), int32_t(last)};
        };
        for (uint32_t c = 0; c < source.columns(); ++c, ++outputColumn) {
            out.offsets[outputColumn] = uint32_t(out.rowSide.size());
            const auto &cov = source.coverage[c];
            const auto bid = project(cov.bidLo, cov.bidHi), ask = project(cov.askLo, cov.askHi);
            out.coverage.push_back({bid.first, bid.second, ask.first, ask.second});
            out.observedMs.push_back(source.observedMs[c]);
            const uint64_t factor = uint64_t(source.nativeFactor[c]) * ratio;
            if (factor > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("joined recording native factor exceeds GPU width");
            out.nativeFactor.push_back(uint32_t(factor));
            out.columnScale.push_back(source.columnScale[c]);
            if (out.sourceMinutes != 1) for (uint32_t side = 0; side < 2; ++side) {
                out.coverageRunOffsets[outputColumn * 2 + side] = uint32_t(out.coverageRuns.size());
                for (uint32_t j = source.coverageRunOffsets[c * 2 + side];
                     j < source.coverageRunOffsets[c * 2 + side + 1]; ++j) {
                    const auto &run = source.coverageRuns[j];
                    const auto projected = project(run[0], run[1]);
                    if (projected.first <= projected.second)
                        out.coverageRuns.push_back({projected.first, projected.second, run[2], run[3]});
                }
            }
            for (uint32_t i = source.offsets[c]; i < source.offsets[c + 1]; ++i) {
                const auto row = checkedSub(scaledRow(source.baseRow, source.rowSide[i] & 0x7fffffffu, ratio),
                                            out.baseRow);
                if (row < 0 || row > 0x7fffffffu)
                    throw std::runtime_error("joined recording entry exceeds GPU row width");
                out.rowSide.push_back(uint32_t(row) | (source.rowSide[i] & 0x80000000u));
                out.code.push_back(source.code[i]);
                if (out.sourceMinutes != 1) out.entryCoveredMs.push_back(source.entryCoveredMs[i]);
            }
        }
    };
    append(older, olderRatio);
    append(recent, recentRatio);
    out.offsets.back() = uint32_t(out.rowSide.size());
    if (out.sourceMinutes != 1)
        out.coverageRunOffsets.back() = uint32_t(out.coverageRuns.size());
    out.decodeMs = older.decodeMs + recent.decodeMs;
    out.loadMs = older.loadMs + recent.loadMs + elapsed(started);
    return out;
}

RecordingEntries syntheticRecordingEntries(uint32_t count) {
    const auto started = Clock::now();
    if (!count || count > 100'000'000) throw std::invalid_argument("synthetic entry count out of range");
    RecordingEntries out;
    out.startMs = kHmc2MinMs;
    out.nativeTick = 1; out.priceScale = 100;
    constexpr uint32_t cols = 1000;
    const uint32_t rows = (count + cols - 1) / cols;
    out.coverage.resize(cols, {0, int32_t(rows - 1), 0, int32_t(rows - 1)});
    out.observedMs.resize(cols, 60'000);
    out.nativeFactor.resize(cols, 1);
    out.columnScale.resize(cols, out.sizeScale);
    out.offsets.resize(cols + 1);
    out.rowSide.reserve(count); out.code.reserve(count);
    for (uint32_t c = 0; c < cols; ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.rowSide.size());
        for (uint32_t r = 0; r < rows && out.rowSide.size() < count; ++r) {
            out.rowSide.push_back(r | ((r > rows / 2 ? 1u : 0u) << 31));
            const bool wall = r == rows / 3 || r == rows * 2 / 3 || r == rows / 2 + 41;
            out.code.push_back(encodeSize(wall ? 116'000.0 : 0.01 + ((r * 17u + c * 13u) % 100u) * 0.003,
                                          out.sizeScale));
        }
    }
    out.offsets.back() = count;
    out.loadMs = elapsed(started);
    return out;
}

BinCell binRecordingCell(const RecordingEntries &data, uint32_t first, uint32_t end,
                         uint32_t rowLo, uint32_t rowHi) {
    if (first >= end || end > data.columns() || rowLo > rowHi) return {};
    double bid = 0, ask = 0, duration = 0;
    bool valid = true;
    auto coverageMs = [&](uint32_t c, bool askSide, uint32_t row) -> uint32_t {
        if (data.sourceMinutes == 1) {
            const auto &cov = data.coverage[c];
            return (askSide ? (cov.askLo <= int64_t(row) && cov.askHi >= int64_t(row)) :
                              (cov.bidLo <= int64_t(row) && cov.bidHi >= int64_t(row))) ?
                data.observedMs[c] : 0;
        }
        const uint32_t side = askSide ? 1u : 0u;
        for (uint32_t i = data.coverageRunOffsets[c * 2 + side];
             i < data.coverageRunOffsets[c * 2 + side + 1]; ++i) {
            const auto &run = data.coverageRuns[i];
            if (run[0] <= int64_t(row) && run[1] >= int64_t(row)) return uint32_t(run[2]);
        }
        return 0;
    };
    for (uint32_t c = first; c < end; ++c) {
        if (!data.observedMs[c]) continue;
        const auto &cov = data.coverage[c];
        if (data.sourceMinutes == 1)
            valid &= cov.bidLo <= int64_t(rowLo) && cov.bidHi >= int64_t(rowHi) &&
                     cov.askLo <= int64_t(rowLo) && cov.askHi >= int64_t(rowHi);
        else
            for (uint32_t row = rowLo; row <= rowHi; ++row)
                valid &= coverageMs(c, false, row) == data.observedMs[c] &&
                         coverageMs(c, true, row) == data.observedMs[c];
        duration += data.observedMs[c];
        for (uint32_t i = data.offsets[c]; i < data.offsets[c + 1]; ++i) {
            const auto row = data.rowSide[i] & 0x7fffffffu;
            if (row < rowLo || row > rowHi) continue;
            const double amount = decodeSize(uint16_t(data.code[i]), data.columnScale[c]) *
                (data.sourceMinutes == 1 ? data.observedMs[c] : data.entryCoveredMs[i]);
            (data.rowSide[i] & 0x80000000u ? ask : bid) += amount;
        }
    }
    if (!duration) return {};
    const uint32_t group = rowHi - rowLo + 1;
    for (uint32_t c = first; c < end; ++c) {
        if (!data.observedMs[c]) continue;
        const uint32_t factor = data.nativeFactor[c];
        if (!factor || group % factor || (data.baseRow + rowLo) % factor) return {};
    }
    if (!valid && !data.preNormalized && (end - first > 1 || data.sourceMinutes != 1)) {
        bid = ask = 0;
        for (uint32_t c = first; c < end; ++c) {
            if (!data.observedMs[c]) continue;
            for (uint32_t i = data.offsets[c]; i < data.offsets[c + 1]; ++i) {
                const uint32_t row = data.rowSide[i] & 0x7fffffffu;
                if (row < rowLo || row > rowHi) continue;
                const bool isAsk = (data.rowSide[i] & 0x80000000u) != 0;
                double covered = 0, gridMs = 0;
                for (uint32_t d = first; d < end; ++d) {
                    if (!data.observedMs[d] || data.nativeFactor[d] != data.nativeFactor[c]) continue;
                    gridMs += data.observedMs[d];
                    covered += coverageMs(d, isAsk, row);
                }
                if (covered > 0)
                    (isAsk ? ask : bid) += decodeSize(uint16_t(data.code[i]), data.columnScale[c]) *
                        (data.sourceMinutes == 1 ? data.observedMs[c] : data.entryCoveredMs[i]) /
                        covered * gridMs;
            }
        }
    }
    return {float(bid / duration), float(ask / duration), valid};
}
} // namespace recording
