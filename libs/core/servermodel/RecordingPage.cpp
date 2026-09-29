#include "RecordingPage.hpp"
#include "RecordingLive.hpp"
#include "PriceLadder.hpp"
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>

namespace recording {
namespace {
constexpr int64_t minute = 60'000, hour = 3'600'000;
double gridPosition(double price, double tick) {
    const double value = price / tick;
    const double nearest = std::round(value);
    return std::abs(value - nearest) <= 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(value))
               ? nearest : value;
}
bool multiple(double a, double b) {
    const auto ratio = a / b;
    return std::isfinite(ratio) && ratio >= 1 && std::abs(ratio - std::round(ratio)) < 1e-8;
}
BuildStatus status(ReadStatus s) {
    switch (s) {
    case ReadStatus::Budget: return BuildStatus::Budget;
    case ReadStatus::Cancelled: return BuildStatus::Cancelled;
    case ReadStatus::IoError: return BuildStatus::IoError;
    default: return BuildStatus::Complete;
    }
}
struct GridError : std::runtime_error { using std::runtime_error::runtime_error; };
using RowSums = std::vector<std::array<long double, 2>>;
struct NativeAggregate {
    ServedColumn column;
    double nativeTick = 0;
    bool direct = false;
    std::vector<std::array<long double, 2>> directSums;
    std::unordered_map<int64_t, std::array<long double, 2>> numerator;
    std::array<std::map<int64_t, int64_t>, 2> coverage;
    void add(const Hmc2Record &r, const PriceBand &band, ReadControl &control) {
        const double tick = r.header.rowTickUnits / r.header.priceScale;
        nativeTick = tick;
        column.observedMs += r.observedMs;
        column.flags |= r.flags;
        const auto lo = static_cast<int64_t>(std::llround(band.lo / tick));
        const auto hi = static_cast<int64_t>(std::llround((band.lo + band.rows * band.tick) / tick)) - 1;
        auto cover = [&](int64_t low, int64_t high, bool ask, uint32_t ms) {
            low = std::max(low, lo);
            high = std::min(high, hi);
            if (low <= high) {
                coverage[ask][low] += ms;
                coverage[ask][high + 1] -= ms;
            }
        };
        if (r.header.tfMs == hour && !(r.flags & kApproximateCoverage)) {
            for (const auto &run : r.coverage)
                cover(run.lo, run.hi, run.isAsk, run.coveredMs);
        } else {
            cover(r.bidRowLo, r.bidRowHi, false, r.observedMs);
            cover(r.askRowLo, r.askRowHi, true, r.observedMs);
        }
        if (direct)
            directSums.resize(band.rows);
        const auto group = static_cast<int64_t>(std::llround(band.tick / tick));
        size_t n = 0;
        for (const auto &e : r.entries) {
            if ((++n & 255) == 0 && !control.poll())
                return;
            if (e.row < lo || e.row > hi)
                continue;
            if (r.header.tfMs == minute &&
                (e.row < (e.isAsk ? r.askRowLo : r.bidRowLo) || e.row > (e.isAsk ? r.askRowHi : r.bidRowHi)))
                continue;
            const auto ms = r.header.tfMs == hour ? e.coveredMs : r.observedMs;
            const auto size = decodeSize(e.twapCode, r.header.sizeScale);
            if (!std::isfinite(size))
                throw std::runtime_error("nonfinite decoded recording quantity");
            if (direct) {
                const auto row = band.rows - 1 - static_cast<uint32_t>((e.row - lo) / group);
                directSums[row][e.isAsk] += size;
            } else
                numerator[e.row][e.isAsk] += static_cast<long double>(size) * ms;
        }
    }
    RowSums finish(const PriceBand &band, ReadControl &control) {
        column.validity.resize((band.rows + 7) / 8);
        auto sums = direct ? std::move(directSums) : std::vector<std::array<long double, 2>>(band.rows);
        // Turn interval differences into the per-native-row denominator. Zero
        // rows participate even when they never appear in the sparse entries.
        for (auto &side : coverage) {
            int64_t total = 0;
            for (auto &[edge, delta] : side) {
                total += delta;
                delta = total;
            }
        }
        auto covered = [&](bool ask, int64_t row) -> int64_t {
            auto it = coverage[ask].upper_bound(row);
            return it == coverage[ask].begin() ? 0 : std::prev(it)->second;
        };
        const auto nativeLo = static_cast<int64_t>(std::llround(band.lo / nativeTick));
        const auto group = static_cast<int64_t>(std::llround(band.tick / nativeTick));
        size_t visited = 0;
        for (const auto &[row, values] : numerator) {
            if ((++visited & 255) == 0 && !control.poll())
                return {};
            const auto index = band.rows - 1 - static_cast<uint32_t>((row - nativeLo) / group);
            for (bool ask : {false, true}) {
                const auto ms = covered(ask, row);
                if (ms)
                    sums[index][ask] += values[ask] / ms;
            }
        }
        auto fullyCovered = [&](bool ask, int64_t lo, int64_t end) {
            if (covered(ask, lo) != static_cast<int64_t>(column.observedMs))
                return false;
            for (auto it = coverage[ask].upper_bound(lo); it != coverage[ask].end() && it->first < end; ++it)
                if (it->second != static_cast<int64_t>(column.observedMs))
                    return false;
            return true;
        };
        for (uint32_t row = 0; row < band.rows; ++row) {
            const auto lo = nativeLo + (band.rows - 1 - row) * group;
            if (fullyCovered(false, lo, lo + group) && fullyCovered(true, lo, lo + group))
                column.validity[row / 8] |= static_cast<uint8_t>(1u << (row % 8));
        }
        return sums;
    }
};
// Preserve each grid's native-row denominators, then combine its display-row
// quantities by observed duration. Never reinterpret a native row on another grid.
struct Aggregate {
    ServedColumn column;
    bool direct = false;
    bool incompatible = false;
    std::map<double, NativeAggregate> grids;
    void add(const Hmc2Record &r, const PriceBand &band, ReadControl &control) {
        column.observedMs += r.observedMs;
        column.flags |= r.flags;
        const double tick = r.header.rowTickUnits / r.header.priceScale;
        if (!multiple(band.tick, tick) || gridPosition(band.lo, tick) != std::round(band.lo / tick)) {
            incompatible = true;
            grids.clear();
        }
        if (incompatible) return;
        auto &grid = grids[tick];
        grid.direct = direct;
        grid.add(r, band, control);
    }
    ServedColumn finish(const PriceBand &band, const SizeScale &scale, int64_t tf, ReadControl &control) {
        column.flags &= ~kPartial;
        if (column.observedMs < static_cast<uint64_t>(tf)) column.flags |= kPartial;
        column.cells.resize(band.rows);
        column.quantities.resize(band.rows);
        column.validity.resize((band.rows + 7) / 8);
        // A constituent that cannot be represented makes this output bucket
        // unknown, including quantities; other buckets on the page still serve.
        if (incompatible) return std::move(column);
        RowSums sums;
        bool first = true;
        for (auto &[tick, grid] : grids) {
            auto values = grid.finish(band, control);
            if (!control.poll()) return {};
            if (first) {
                sums = std::move(values);
                column.validity = grid.column.validity;
            } else {
                for (size_t i = 0; i < column.validity.size(); ++i)
                    column.validity[i] &= grid.column.validity[i];
            }
            const auto weight = static_cast<long double>(grid.column.observedMs) / column.observedMs;
            for (uint32_t row = 0; row < band.rows; ++row)
                for (bool ask : {false, true})
                    if (first) sums[row][ask] *= weight;
                    else sums[row][ask] += values[row][ask] * weight;
            first = false;
        }
        double maximum = 0;
        for (uint32_t row = 0; row < band.rows; ++row) {
            const bool ask = sums[row][1] > sums[row][0];
            const auto quantity = static_cast<double>(sums[row][ask]);
            if (!std::isfinite(quantity))
                throw std::runtime_error("nonfinite aggregated recording quantity");
            column.cells[row] = withSide(encodeSize(quantity, scale), ask);
            maximum = std::max(maximum, quantity);
        }
        column.quantityScale = maximum / 65535.0;
        if (maximum > 0)
            for (uint32_t row = 0; row < band.rows; ++row)
                column.quantities[row] = static_cast<uint16_t>(std::clamp(std::llround(
                    static_cast<double>(std::max(sums[row][0], sums[row][1])) / column.quantityScale), 0LL, 65535LL));
        return std::move(column);
    }
};
}
BuildResult buildPage(Hmc2Reader &reader, const BuildRequest &q, StopToken stop) {
    BuildResult out;
    ReadControl control{q.budgets, stop};
    auto done = [&]() {
        if (out.status == BuildStatus::Complete)
            out.status = status(control.status);
        out.sourceRecords = control.sourceRecords;
        out.entriesVisited = control.entriesVisited;
        std::reverse(out.columns.begin(), out.columns.end());
        return out;
    };
    if (q.symbol.empty() || !q.count || q.count > 1024 || !q.rows || q.rows > 16384 ||
        q.tfMs < minute || q.tfMs > kHmc2EndMs - kHmc2MinMs || q.tfMs % minute ||
        (q.tfMs >= hour && q.tfMs % hour) || !std::isfinite(q.priceLo) || !std::isfinite(q.priceHi) ||
        q.priceLo < 0 || q.priceHi <= q.priceLo || q.priceHi > 1e12 || q.endMs < 0 || q.endMs >= kHmc2EndMs ||
        (q.displayTick && (!std::isfinite(*q.displayTick) || *q.displayTick <= 0))) {
        out.status = BuildStatus::InvalidRequest;
        out.message = "invalid timeframe, count, band, timestamp or display tick";
        return done();
    }
    try {
        SeriesAvailability near;
        if (q.tfMs < hour)
            near = reader.availability(q.symbol, "near", minute, control);
        auto deep = reader.availability(q.symbol, "deep", minute, control);
        SeriesAvailability hours;
        if (q.tfMs >= hour)
            hours = reader.availability(q.symbol, "deep", hour, control);
        if (!control.poll())
            return done();
        const auto deepHeader = deep.latestHeader ? deep.latestHeader : hours.latestHeader;
        const double deepTick = deepHeader ? deepHeader->rowTickUnits / deepHeader->priceScale : 5;
        const double nearTick = near.latestHeader ? near.latestHeader->rowTickUnits / near.latestHeader->priceScale : 1;
        // Hourly history is the persisted deep layer. Never expand deep rows into near rows.
        if (!std::isfinite(deepTick) || !std::isfinite(nearTick) || deepTick <= 0 || nearTick <= 0)
            throw GridError("invalid source price grid");
        const bool baseDeep = q.tfMs >= hour || deepTick <= nearTick;
        const double baseTick = baseDeep ? deepTick : nearTick;
        double tick = q.displayTick.value_or(baseTick);
        auto rowCount = [&] { return std::ceil(gridPosition(q.priceHi, tick)) - std::floor(gridPosition(q.priceLo, tick)); };
        auto chooseTick = [&](const std::optional<Hmc2Header> &header, double fallback, double minimum) {
            const double scale = header ? header->priceScale : 100;
            const int64_t native = header ? header->rowTickUnits : static_cast<int64_t>(fallback * scale);
            auto units = ladderTickUnits(minimum * scale, native);
            for (;;) {
                if (!units) throw GridError("band cannot be represented on the native price ladder");
                tick = units / scale;
                if (rowCount() <= q.rows) break;
                units = ladderTickUnits(std::nextafter(static_cast<double>(units),
                                                       std::numeric_limits<double>::infinity()), native);
            }
        };
        if (!q.displayTick) {
            chooseTick(baseDeep ? deepHeader : near.latestHeader, baseTick,
                       std::max(baseTick, (q.priceHi - q.priceLo) / q.rows));
            // The selected deep grid may not divide the near-grid choice.
            if (tick >= deepTick && !multiple(tick, deepTick))
                chooseTick(deepHeader, deepTick, tick);
        }
        out.layer = tick < deepTick ? "near" : "deep";
        const auto &source = out.layer == "near" ? near : deep;
        const auto nativeTick = out.layer == "near" ? nearTick : deepTick;
        if (rowCount() > q.rows || rowCount() < 1 || !multiple(tick, nativeTick) ||
            (q.tfMs >= hour && out.layer != "deep") || (std::ceil(gridPosition(q.priceHi, tick)) * tick) / nativeTick > 0x1p52) {
            out.status = BuildStatus::InvalidRequest;
            out.message = "display tick must cover the band, be a native-tick multiple, and use deep for hours";
            return done();
        }
        out.band = {std::floor(gridPosition(q.priceLo, tick)) * tick, tick, static_cast<uint32_t>(rowCount())};
        if (source.latestHeader)
            out.sizeScale = source.latestHeader->sizeScale;
        else if (hours.latestHeader)
            out.sizeScale = hours.latestHeader->sizeScale;
        std::optional<int64_t> oldest = source.oldestMs, latest = source.latestMs;
        int64_t tail = kHmc2EndMs;
        if (q.tfMs >= hour) {
            oldest = hours.oldestMs;
            latest = hours.latestMs;
            if (deep.latestMs) {
                const auto candidate = *deep.latestMs / hour * hour;
                if (!hours.latestMs || candidate > *hours.latestMs) {
                    tail = candidate;
                    oldest = oldest ? std::min(*oldest, tail) : tail;
                    latest = *deep.latestMs;
                }
            }
        }
        if (!oldest || !latest) {
            out.exhausted = true;
            return done();
        }
        out.oldestAvailableMs = *oldest / q.tfMs * q.tfMs;
        out.latestAvailableMs = *latest / q.tfMs * q.tfMs;
        const auto endBucket = q.endMs ? q.endMs / q.tfMs * q.tfMs : *out.latestAvailableMs;
        out.scannedStartMs = out.scannedEndMs = std::min(kHmc2EndMs, endBucket + q.tfMs);
        out.nextEnd = endBucket;
        // Walk output buckets backward so every published page is a proven suffix.
        // A budget/cancellation in a rollup drops that unfinished bucket entirely.
        for (uint32_t n = 0; n < q.count; ++n) {
            const auto bucket = endBucket - static_cast<int64_t>(n) * q.tfMs;
            if (bucket < *out.oldestAvailableMs) {
                out.exhausted = true;
                break;
            }
            if (!control.poll())
                break;
            std::optional<ReadControl::WorkScope> firstColumn;
            if (out.columns.empty() && q.tfMs / (q.tfMs < hour ? minute : hour) <= 16)
                firstColumn.emplace(control, false);
            Aggregate aggregate;
            aggregate.direct = q.tfMs == minute;
            aggregate.column.bucketStartMs = bucket;
            auto add = [&](const Hmc2Record &r) { aggregate.add(r, out.band, control); };
            if (q.tfMs < hour) {
                reader.visit(q.symbol, out.layer, minute, bucket, bucket + q.tfMs, add, control);
            } else {
                reader.visit(q.symbol, out.layer, hour, bucket, std::min(bucket + q.tfMs, tail), add, control);
                if (tail < bucket + q.tfMs)
                    reader.visit(q.symbol, out.layer, minute, std::max(bucket, tail), bucket + q.tfMs, add, control);
            }
            if (!control.poll())
                break;
            if (aggregate.column.observedMs) {
                auto column = aggregate.finish(out.band, out.sizeScale, q.tfMs, control);
                if (!control.poll())
                    break;
                out.columns.push_back(std::move(column));
            }
            out.scannedStartMs = std::max(bucket, kHmc2MinMs);
            out.nextEnd = bucket - q.tfMs;
            if (bucket == *out.oldestAvailableMs) {
                out.exhausted = true;
                break;
            }
        }
    } catch (const GridError &e) {
        out.status = BuildStatus::IncompatibleGrid;
        out.message = e.what();
    } catch (const std::exception &e) {
        out.status = BuildStatus::IoError;
        out.message = e.what();
    }
    return done();
}
BuildResult buildPage(const std::filesystem::path &root, const BuildRequest &q, StopToken stop) {
    Hmc2Reader reader(root);
    return buildPage(reader, q, stop);
}

struct LiveBuilder::Impl {
    LiveView view;
    struct Bucket {
        Aggregate aggregate;
        int64_t through = 0;
        std::optional<ServedColumn> final;
    };
    std::map<int64_t, Bucket> buckets;
    explicit Impl(LiveView v) : view(std::move(v)) {}
};
LiveBuilder::LiveBuilder(LiveView view) : impl_(std::make_unique<Impl>(std::move(view))) {}
LiveBuilder::~LiveBuilder() = default;
BuildResult LiveBuilder::build(Hmc2Reader &reader, const LiveCache::Snapshot &source,
                               int64_t deliveredFinalThroughMs) {
    auto &v = impl_->view;
    BuildResult out;
    out.band = v.band;
    out.layer = v.layer;
    if (v.tfMs < minute || v.tfMs > 86'400'000 || v.tfMs % minute ||
        !v.band.rows || v.band.rows > 16384 || !std::isfinite(v.band.lo) || v.band.lo < 0 ||
        !std::isfinite(v.band.tick) || v.band.tick <= 0 ||
        !std::isfinite(v.band.lo + v.band.rows * v.band.tick) ||
        v.band.lo + v.band.rows * v.band.tick > 1e12) {
        out.status = BuildStatus::InvalidRequest;
        out.message = "invalid live timeframe or band";
        return out;
    }
    const auto latestCommit = source.committed.empty() ? RecordPtr{} : source.committed.back();
    const auto provisional = source.provisional.empty() ? RecordPtr{} : source.provisional.rbegin()->second;
    const auto newest = provisional && (!latestCommit ||
        provisional->bucketStartMs > latestCommit->bucketStartMs) ? provisional : latestCommit;
    if (!newest) return out;
    out.sizeScale = newest->header.sizeScale;
    const double nativeTick = newest->header.rowTickUnits / newest->header.priceScale;
    if (!(nativeTick > 0) || !std::isfinite(nativeTick) ||
        (v.band.lo + v.band.rows * v.band.tick) / nativeTick > 0x1p52 ||
        v.band.rows * v.band.tick / nativeTick > LiveCache::kMaxEntries) {
        out.status = BuildStatus::InvalidRequest;
        out.message = "live band exceeds 262144 native rows";
        return out;
    }
    const auto current = newest->bucketStartMs / v.tfMs * v.tfMs;
    out.latestAvailableMs = current;
    // Also correct the preceding bucket when its delayed final commit arrives.
    std::vector<int64_t> wanted;
    if (current - v.tfMs >= kHmc2MinMs) wanted.push_back(current - v.tfMs);
    wanted.push_back(current);
    for (auto it = impl_->buckets.begin(); it != impl_->buckets.end();) {
        if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) it = impl_->buckets.erase(it);
        else ++it;
    }
    ReadControl control{{4096, 2'000'000, 100}};
    try {
        for (const auto start : wanted) {
            auto [it, fresh] = impl_->buckets.try_emplace(start);
            auto &b = it->second;
            if (fresh) {
                b.through = start;
                b.aggregate.column.bucketStartMs = start;
                b.aggregate.direct = v.tfMs == minute;
            }
            if (b.final) {
                if (b.final->observedMs && start > deliveredFinalThroughMs) out.columns.push_back(*b.final);
                continue;
            }
            const auto end = std::max(start, std::min(start + v.tfMs, source.committedThroughMs));
            // The retained suffix proves all commits since its first record. Older
            // data is scanned once; a budget keeps the proven prefix and resumes.
            const auto cachedStart = source.committed.empty() ? end : source.committed.front()->bucketStartMs;
            const auto diskEnd = std::min(end, cachedStart);
            if (b.through < diskEnd) {
                const auto scan = reader.visit(v.symbol, v.layer, minute, b.through, diskEnd,
                    [&](const Hmc2Record &r) {
                        ReadControl atomic; // one admitted record is integrated completely
                        b.aggregate.add(r, v.band, atomic);
                    }, control);
                b.through = scan.scannedEndMs;
                if (scan.status != ReadStatus::Complete) {
                    out.status = status(scan.status);
                    return out; // no partial warmup is presented as a complete rollup
                }
            }
            for (const auto &r : source.committed) {
                if (r->bucketStartMs < b.through || r->bucketStartMs >= end) continue;
                ReadControl atomic;
                b.aggregate.add(*r, v.band, atomic);
                b.through = r->bucketStartMs + minute;
            }
            b.through = std::max(b.through, end);
            if (b.through == start + v.tfMs) {
                // Final projections are immutable. Finish in place exactly once;
                // release the potentially 262k-row accumulator after projection.
                ReadControl atomic;
                b.final = b.aggregate.column.observedMs
                    ? b.aggregate.finish(v.band, out.sizeScale, v.tfMs, atomic) : ServedColumn{};
                b.aggregate = {};
                if (b.final->observedMs && start > deliveredFinalThroughMs) out.columns.push_back(*b.final);
                continue;
            }
            auto live = b.aggregate; // bounded by distinct native rows, not minute count
            for (const auto &[time, pending] : source.provisional) {
                if (time < b.through || time < start || time >= start + v.tfMs) continue;
                ReadControl atomic;
                live.add(*pending, v.band, atomic);
            }
            // Even a bucket with only committed source records is still forming
            // until the recorder watermark proves its exclusive end.
            if (b.through < start + v.tfMs) live.column.flags |= kProvisional;
            if (live.column.observedMs) {
                ReadControl atomic;
                out.columns.push_back(live.finish(v.band, out.sizeScale, v.tfMs, atomic));
            }
        }
    } catch (const GridError &e) {
        out.status = BuildStatus::IncompatibleGrid; out.message = e.what();
    } catch (const std::exception &e) {
        out.status = BuildStatus::IoError; out.message = e.what();
    }
    out.sourceRecords = control.sourceRecords;
    out.entriesVisited = control.entriesVisited;
    return out;
}
} // namespace recording
