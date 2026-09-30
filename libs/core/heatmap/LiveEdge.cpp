#include "LiveEdge.hpp"
#include "TimeComposer.hpp"
#include <algorithm>
#include <set>

namespace heatmap {
namespace {
using Range = SparseColumns::TimeRange;
void addRange(std::vector<Range> &ranges, int64_t lo, int64_t hi) {
    if (lo >= hi) return;
    ranges.push_back({lo, hi});
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.startMs < b.startMs; });
    size_t n = 0;
    for (const auto r : ranges) {
        if (n && r.startMs <= ranges[n - 1].endMs) ranges[n - 1].endMs = std::max(ranges[n - 1].endMs, r.endMs);
        else ranges[n++] = r;
    }
    ranges.resize(n);
}
bool equal(const SparseColumn &a, const SparseColumn &b) {
    if (a.bucketStartMs != b.bucketStartMs || a.observedMs != b.observedMs || a.flags != b.flags ||
        a.native.size() != b.native.size()) return false;
    for (size_t i = 0; i < a.native.size(); ++i) {
        const auto &x = a.native[i], &y = b.native[i];
        if (x.grid.configHash != y.grid.configHash || x.grid.rowTickUnits != y.grid.rowTickUnits ||
            x.grid.priceScale != y.grid.priceScale || x.sizeScale.floor != y.sizeScale.floor ||
            x.sizeScale.codesPerOctave != y.sizeScale.codesPerOctave || x.baseRow != y.baseRow ||
            x.observedMs != y.observedMs || x.composed != y.composed || x.entryCoveredMs != y.entryCoveredMs ||
            x.numerators != y.numerators || x.entries.size() != y.entries.size()) return false;
        for (size_t j = 0; j < x.entries.size(); ++j)
            if (x.entries[j].rowSide != y.entries[j].rowSide || x.entries[j].code != y.entries[j].code) return false;
        for (size_t side = 0; side < 2; ++side) {
            if (x.coverage[side].size() != y.coverage[side].size()) return false;
            for (size_t j = 0; j < x.coverage[side].size(); ++j) {
                const auto &u = x.coverage[side][j], &v = y.coverage[side][j];
                if (u.lo != v.lo || u.hi != v.hi || u.coveredMs != v.coveredMs) return false;
            }
        }
    }
    return true;
}
int64_t hour(int64_t t) { return recording::floorDiv(t, kHourMs) * kHourMs; }
bool dropCovered(LiveEdgeSnapshot &edge, const ChunkStore &store) {
    const auto before = edge.minutes.size();
    const auto missingBefore = edge.missingMinutes.size();
    auto obsolete = [&](int64_t t) {
        const auto chunk = store.cached({edge.symbol, edge.source, kMinuteMs, hour(t)});
        return t < edge.openEndMs - LiveEdge::kRetainedMinutes * kMinuteMs ||
               (chunk && t < chunk->committedThroughMs);
    };
    std::erase_if(edge.minutes, [&](const auto &entry) { return obsolete(entry.first); });
    std::erase_if(edge.missingMinutes, obsolete);
    edge.proven.clear();
    for (const auto &[t, column] : edge.minutes) addRange(edge.proven, t, t + kMinuteMs);
    return edge.minutes.size() != before || edge.missingMinutes.size() != missingBefore;
}
} // namespace
LiveEdge::LiveEdge(std::string symbol, std::string source) {
    auto initial = std::make_shared<LiveEdgeSnapshot>();
    initial->symbol = std::move(symbol);
    initial->source = std::move(source);
    snapshot_ = std::move(initial);
}
void LiveEdge::newEpoch() { fresh_ = true; }
bool LiveEdge::accept(std::shared_ptr<const ChunkFrame> frame, const ChunkStore &store) {
    if (!frame || frame->kind != ChunkKind::LiveColumn || frame->key.symbol != snapshot_->symbol ||
        frame->key.source != snapshot_->source || (!fresh_ && frame->state.revision <= snapshot_->revision)) return false;
    auto next = std::make_shared<LiveEdgeSnapshot>(*snapshot_);
    if (fresh_) { // old uncommitted data has no proof in the new server epoch
        next->minutes.clear();
        next->missingMinutes.clear();
    }
    next->layer = frame->columns.layer;
    next->revision = frame->state.revision;
    next->committedThroughMs = frame->state.committedThroughMs;
    next->openEndMs = frame->columns.endMs;
    ++next->version;
    std::set<int64_t> received;
    for (const auto &column : frame->columns.columns) received.insert(column.bucketStartMs);
    std::erase_if(next->minutes, [&](const auto &entry) {
        if (!(entry.second->flags & recording::kProvisional) || received.contains(entry.first)) return false;
        next->missingMinutes.insert(entry.first);
        return true;
    });
    for (const auto &column : frame->columns.columns) {
        if (column.bucketStartMs < next->openEndMs - kRetainedMinutes * kMinuteMs) continue;
        next->missingMinutes.erase(column.bucketStartMs);
        auto &slot = next->minutes[column.bucketStartMs];
        // Reuse frozen pending minutes even when another revision transports
        // them again. No inference from kPartial: a just-closed minute may be
        // the newest known provisional until the next publication.
        if (!slot || !equal(*slot, column)) slot = std::make_shared<const SparseColumn>(column);
    }
    dropCovered(*next, store);
    fresh_ = false;
    snapshot_ = std::move(next);
    return true;
}
bool LiveEdge::trim(const ChunkStore &store) {
    auto next = std::make_shared<LiveEdgeSnapshot>(*snapshot_);
    if (!dropCovered(*next, store)) return false;
    ++next->version;
    snapshot_ = std::move(next);
    return true;
}

LiveComposer::Result LiveComposer::compose(const LiveEdgeSnapshot &edge,
        const std::vector<std::shared_ptr<const StoredChunk>> &chunks, int64_t tfMs, int64_t startMs) {
    if (tfMs < kMinuteMs || tfMs > kDayMs || tfMs % kMinuteMs || startMs % tfMs)
        throw std::invalid_argument("invalid live compose timeframe/window");
    if (tfMs_ != tfMs) { chunks_.clear(); buckets_.clear(); prefixes_.clear(); tfMs_ = tfMs; }
    Result result;
    auto &out = result.columns;
    const auto end = recording::floorDiv(edge.openEndMs + tfMs - 1, tfMs) * tfMs;
    out = {edge.symbol, edge.layer, tfMs, startMs, std::max(startMs, end), {}, {}};
    if (startMs >= end) return result;
    std::map<int64_t, std::vector<std::shared_ptr<const SparseColumn>>> inputs;
    std::vector<Range> proven;
    std::map<int64_t, int64_t> cutoffs;
    std::set<int64_t> used;
    for (const auto &chunk : chunks) {
        if (!chunk || chunk->key.levelMs != kMinuteMs || chunk->key.symbol != edge.symbol ||
            chunk->key.source != edge.source) continue;
        const auto &data = *chunk->columns;
        const auto cutoff = chunk->committedThroughMs;
        cutoffs[chunk->key.startMs] = cutoff;
        used.insert(chunk->key.startMs);
        auto &cached = chunks_[chunk->key.startMs];
        if (cached.generation != chunk->generation) {
            cached = {};
            cached.generation = chunk->generation;
            std::map<int64_t, std::vector<const SparseColumn*>> groups;
            for (const auto &column : data.columns) if (column.bucketStartMs < cutoff)
                groups[recording::floorDiv(column.bucketStartMs, tfMs) * tfMs].push_back(&column);
            for (const auto &[bucket, columns] : groups) {
                cached.buckets[bucket].column = std::make_shared<SparseColumn>(composeColumn(columns, bucket, tfMs));
                ++result.committedPieces;
            }
            for (const auto &r : data.scannedRanges)
                for (auto b = recording::floorDiv(r.startMs, tfMs) * tfMs; b < std::min(r.endMs, cutoff); b += tfMs)
                    addRange(cached.buckets[b].proven, std::max(b, r.startMs), std::min({b + tfMs, r.endMs, cutoff}));
        }
        for (const auto &[b, piece] : cached.buckets) if (b >= startMs && b < end) {
            if (piece.column) inputs[b].push_back(piece.column);
            for (const auto &r : piece.proven) addRange(proven, r.startMs, r.endMs);
        }
    }
    std::erase_if(chunks_, [&](const auto &item) { return !used.contains(item.first); });
    for (const auto &[t, column] : edge.minutes) {
        const auto b = recording::floorDiv(t, tfMs) * tfMs;
        if (b < startMs || t >= edge.openEndMs) continue;
        const auto it = cutoffs.find(hour(t));
        if (it != cutoffs.end() && t < it->second) continue; // chunk iff start < stored cutoff
        inputs[b].push_back(column);
        addRange(proven, t, t + kMinuteMs);
    }
    std::set<int64_t> kept;
    // Same forming rule as TimeComposer: prove the terminal known prefix, not
    // the frame's bounding extent (commit may precede the next open publish).
    // An omitted provisional is a known hole, not an unobserved future minute.
    int64_t knownEnd = startMs;
    for (const auto &r : proven) knownEnd = std::max(knownEnd, r.endMs);
    for (const auto t : edge.missingMinutes) {
        const auto it = cutoffs.find(hour(t));
        if (it == cutoffs.end() || t >= it->second) knownEnd = std::max(knownEnd, t + kMinuteMs);
    }
    for (auto b = startMs; b < end; b += tfMs) {
        const auto through = std::min(b + tfMs, knownEnd);
        const bool scanned = b < knownEnd && std::any_of(proven.begin(), proven.end(), [&](const auto &r) {
            return r.startMs <= b && r.endMs >= through;
        });
        if (!scanned) continue; // an unloaded interior minute is never a zero
        addRange(out.scannedRanges, b, b + tfMs);
        const auto it = inputs.find(b);
        if (it == inputs.end()) continue; // a proven recorder gap
        auto &cached = buckets_[b];
        kept.insert(b);
        if (cached.inputs != it->second) {
            cached.inputs = it->second;
            std::vector<const SparseColumn*> pointers;
            auto &prefix = prefixes_[b];
            std::vector<std::shared_ptr<const SparseColumn>> stable(cached.inputs.begin(), std::prev(cached.inputs.end()));
            if (prefix.inputs != stable) {
                prefix.inputs = std::move(stable);
                std::vector<const SparseColumn*> parts;
                for (const auto &column : prefix.inputs) parts.push_back(column.get());
                prefix.column = parts.empty() ? nullptr : std::make_shared<SparseColumn>(composeColumn(parts, b, tfMs));
                ++result.committedPieces;
            }
            if (prefix.column) pointers.push_back(prefix.column.get());
            pointers.push_back(cached.inputs.back().get());
            cached.column = std::make_shared<SparseColumn>(composeColumn(pointers, b, tfMs));
            ++result.composedBuckets;
        }
        out.columns.push_back(*cached.column);
    }
    std::erase_if(buckets_, [&](const auto &item) { return !kept.contains(item.first); });
    std::erase_if(prefixes_, [&](const auto &item) { return !kept.contains(item.first); });
    validate(out);
    return result;
}
size_t LiveComposer::bytes() const {
    size_t bytes = sizeof(*this);
    std::set<const SparseColumn*> columns;
    for (const auto &[key, parts] : chunks_) {
        bytes += sizeof(parts);
        for (const auto &[b, piece] : parts.buckets) {
            bytes += sizeof(piece) + piece.proven.capacity() * sizeof(Range);
            if (piece.column) columns.insert(piece.column.get());
        }
    }
    for (const auto *cache : {&buckets_, &prefixes_})
        for (const auto &[b, bucket] : *cache) {
            bytes += sizeof(bucket) + bucket.inputs.capacity() * sizeof(std::shared_ptr<const SparseColumn>);
            if (bucket.column) columns.insert(bucket.column.get());
            for (const auto &c : bucket.inputs) columns.insert(c.get());
        }
    for (const auto *c : columns) {
        bytes += sizeof(*c) + c->native.capacity() * sizeof(NativeColumn);
        for (const auto &n : c->native)
            bytes += n.entries.capacity() * sizeof(SparseEntry) + n.entryCoveredMs.capacity() * sizeof(uint64_t) +
                     n.numerators.capacity() * sizeof(long double) +
                     (n.coverage[0].capacity() + n.coverage[1].capacity()) * sizeof(CoverageRun);
    }
    return bytes;
}
} // namespace heatmap
