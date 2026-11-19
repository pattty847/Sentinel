#include "heatmap/BinCell.hpp"
#include "heatmap/HeatmapSpanPlanner.hpp"
#include <gtest/gtest.h>
#include <random>
#include <set>

namespace {
using namespace heatmap;
constexpr int64_t tf = kMinuteMs, tileMs = tf * tiles::kTileColumns;
std::vector<SourceAvailability> availability() {
    std::vector<SourceAvailability> out;
    for (const auto &source : kChunkSources) out.push_back({std::string(source.id), {0, 100 * tileMs, 0}});
    return out;
}
TEST(HeatmapSpanPlanner, BothSourcesAndTwoTilePrefetchInStrictRankOrder) {
    const auto sources = availability();
    const auto plan = planSpans("BTC-USD", tf, 10 * tileMs + tf, 11 * tileMs - tf, sources);
    ASSERT_EQ(plan.size(), 5u);
    EXPECT_EQ(plan[0].id.tile, 10);
    EXPECT_EQ(plan[0].rank.tier, SpanTier::Visible);
    std::set<int64_t> tiles;
    for (size_t i = 0; i < plan.size(); ++i) {
        tiles.insert(plan[i].id.tile);
        if (i) EXPECT_LE(plan[i - 1].rank, plan[i].rank);
        if (i) EXPECT_GE(plan[i - 1].rank.fetchPriority(), plan[i].rank.fetchPriority());
        ASSERT_EQ(plan[i].sources.size(), kChunkSources.size());
        for (const auto &source : plan[i].sources) {
            const auto *descriptor = findChunkSource(source.source);
            ASSERT_TRUE(descriptor);
            const tiles::Availability time{0, 100 * tileMs, 0};
            EXPECT_EQ(source.chunks, tiles::chunksFor("BTC-USD", source.source, tf, plan[i].id.startMs(),
                                                      plan[i].id.endMs(), time));
            EXPECT_EQ(source.availableStartMs, plan[i].id.startMs());
            EXPECT_EQ(source.availableEndMs, plan[i].id.endMs());
            for (const auto &key : source.chunks) EXPECT_EQ(key.source, source.source);
        }
    }
    EXPECT_EQ(tiles, (std::set<int64_t>{8, 9, 10, 11, 12}));
    // Adjacent prefetch (distance 1) outranks the next ring on both sides.
    EXPECT_EQ(plan[1].rank, (SpanRank{SpanTier::Prefetch, 1}));
    EXPECT_EQ(plan[2].rank, (SpanRank{SpanTier::Prefetch, 1}));
    EXPECT_EQ(plan[3].rank, (SpanRank{SpanTier::Prefetch, 2}));
    // A 4-tile view prefetches one view width (4 tiles) per side.
    const auto wide = planSpans("BTC-USD", tf, 10 * tileMs, 14 * tileMs, sources);
    ASSERT_EQ(wide.size(), 12u);
    const auto [lo, hi] = std::minmax_element(wide.begin(), wide.end(),
                                              [](auto &a, auto &b) { return a.id.tile < b.id.tile; });
    EXPECT_EQ(lo->id.tile, 6);
    EXPECT_EQ(hi->id.tile, 17);
}
TEST(HeatmapSpanPlanner, AvailabilityClipsSourcesAndHourTailIsSourceAware) {
    auto sources = availability();
    for (auto &source : sources) source.time.hourThroughMs = kDayMs;
    const auto plan = planSpans("BTC-USD", kHourMs, 0, 2 * kDayMs, sources);
    ASSERT_FALSE(plan.empty());
    for (const auto &source : plan[0].sources) {
        const auto *descriptor = findChunkSource(source.source);
        ASSERT_TRUE(descriptor);
        EXPECT_EQ(source.chunks.front().levelMs, kHourMs) << "every source has the 1h level";
        EXPECT_EQ(source.chunks.back().levelMs, kMinuteMs);
    }
    // A source that starts later is left out of the spans before it.
    sources[0].time.oldestMs = 11 * tileMs + 5 * tf;
    const auto late = planSpans("BTC-USD", tf, 10 * tileMs, 12 * tileMs, sources);
    for (const auto &span : late) {
        const bool before = span.id.endMs() <= sources[0].time.oldestMs;
        EXPECT_EQ(span.sources.size(), before ? 1u : 2u) << span.id.tile;
        if (span.id.tile == 11) {
            const auto &clipped = span.sources[0];
            EXPECT_EQ(clipped.source, sources[0].source);
            EXPECT_EQ(clipped.availableStartMs, sources[0].time.oldestMs);
        }
    }
}
TEST(HeatmapSpanPlanner, RollupChunksOnlyInsideTheAdvertisedIntervals) {
    constexpr int64_t day0 = 20000 * kDayMs;
    using Keys = std::vector<std::pair<int64_t, int64_t>>; // (level, start - day0)
    auto keys = [](const tiles::Availability &a, int64_t tfMs, const char *source = "hmc2.deep",
                   int64_t from = day0, int64_t to = day0 + 4 * kDayMs) {
        Keys out;
        for (const auto &k : tiles::chunksFor("BTC-USD", source, tfMs, from, to, a))
            out.emplace_back(k.levelMs, k.startMs - day0);
        return out;
    };
    auto minutes = [](Keys &out, int64_t from, int64_t to) {
        for (int64_t t = from; t < to; t += kHourMs) out.emplace_back(kMinuteMs, t);
    };
    // Complete store: hours from the first recorded hour through the last closed day.
    tiles::Availability a{day0 + 2 * kHourMs, day0 + 3 * kDayMs + 7 * kHourMs, day0 + 2 * kHourMs,
                          day0 + 2 * kHourMs, day0 + 3 * kDayMs};
    Keys expected{{kHourMs, 0}, {kHourMs, kDayMs}, {kHourMs, 2 * kDayMs}};
    minutes(expected, 3 * kDayMs, 3 * kDayMs + 7 * kHourMs);
    EXPECT_EQ(keys(a, kHourMs), expected);
    EXPECT_EQ(keys(a, kHourMs, "hmc2.near"), expected) << "every source has the stored levels";
    EXPECT_EQ(keys(a, 4 * kHourMs), expected) << "4h composes 1h";
    // Backfill in progress: hours only for built days; earlier days use minutes.
    a.hourOldestMs = day0 + kDayMs;
    expected.clear();
    minutes(expected, 2 * kHourMs, kDayMs);
    expected.insert(expected.end(), {{kHourMs, kDayMs}, {kHourMs, 2 * kDayMs}});
    minutes(expected, 3 * kDayMs, 3 * kDayMs + 7 * kHourMs);
    EXPECT_EQ(keys(a, kHourMs), expected);
    // A range that ends before the interval uses no hour chunk.
    expected.clear();
    minutes(expected, 2 * kHourMs, 20 * kHourMs);
    EXPECT_EQ(keys(a, kHourMs, "hmc2.deep", day0, day0 + 20 * kHourMs), expected);
    // Sub-hour and non-dividing timeframes compose minutes only.
    expected.clear();
    minutes(expected, 2 * kHourMs, 3 * kDayMs + 7 * kHourMs);
    EXPECT_EQ(keys(a, 15 * kMinuteMs), expected);
    EXPECT_EQ(keys(a, 90 * kMinuteMs), expected);
    // No rollups advertised (through 0): minutes.
    a.hourThroughMs = 0;
    EXPECT_EQ(keys(a, kHourMs), expected);
}
TEST(HeatmapSpanPlanner, RetainedSpansKeepTheirTierAfterPrefetch) {
    const auto sources = availability();
    std::vector<PlannedSpan> retained{{{"BTC-USD", 5 * tf, 1}, {SpanTier::RecentTf, 0}, {}},
                                      {{"BTC-USD", 5 * tf, 2}, {SpanTier::Fallback, 0}, {}}};
    const auto plan = planSpans("BTC-USD", tf, 10 * tileMs, 11 * tileMs, sources, retained);
    ASSERT_EQ(plan.size(), 7u);
    EXPECT_EQ(plan[0].rank.tier, SpanTier::Visible);
    EXPECT_EQ(plan[1].rank.tier, SpanTier::Fallback);
    EXPECT_EQ(plan[1].id.tfMs, 5 * tf);
    EXPECT_EQ(plan.back().rank.tier, SpanTier::RecentTf);
    EXPECT_GT(SpanRank{SpanTier::Fallback}.fetchPriority(), (SpanRank{SpanTier::Prefetch, 1}).fetchPriority());
}

SparseColumns data(int64_t tick, int64_t lo, int64_t end) {
    SparseColumns out{"BTC-USD", "test", tf, 0, 2 * tf, {}, {}};
    out.scannedRanges = {{0, 2 * tf}};
    for (int64_t t = 0; t < 2 * tf; t += tf) {
        NativeColumn n;
        n.grid = {1, tick, 100};
        n.observedMs = tf;
        n.coverage[0] = {{lo / tick, end / tick - 1, tf}};
        n.coverage[1] = n.coverage[0];
        out.columns.push_back({t, tf, 0, {n}});
    }
    return out;
}
// Source ids are arbitrary: nothing may depend on hmc2.near / hmc2.deep.
ResolutionSummary summary(const SparseColumns &fine, const SparseColumns &coarse) {
    auto out = summarizeResolution(fine, "any.fine", tf, 0, 2 * tf);
    mergeResolution(out, summarizeResolution(coarse, "any.coarse", tf, 0, 2 * tf));
    return out;
}
TEST(HeatmapSpanPlanner, AutoReachesTheFineTickOnlyWhenRowsInViewAreInsideTheBand) {
    const auto fine = data(100, 9500, 10500), coarse = data(500, 0, 20000);
    const auto s = summary(fine, coarse);
    ASSERT_EQ(s.columns.size(), 2u);
    ASSERT_EQ(s.columns[0].sources.size(), 2u);
    EXPECT_EQ(s.columns[0].sources[0].nativeTicks, (std::vector<int64_t>{100}));
    EXPECT_EQ(s.columns[0].sources[0].bands, (std::vector<PriceRange>{{9500, 10500}}));
    // Rows in view inside the fine band: $1 (100 units) is reachable.
    EXPECT_EQ(autoTickUnits(s, 0, 0, 2 * tf, 96, 104, 100), 100);
    EXPECT_TRUE(buildsInView(s, 100, 0, 2 * tf, 95, 105));
    // Rows beyond the band: only presets the coarse source builds.
    EXPECT_FALSE(buildsInView(s, 100, 0, 2 * tf, 94, 104));
    EXPECT_FALSE(buildsInView(s, 200, 0, 2 * tf, 94, 104));
    EXPECT_EQ(autoTickUnits(s, 100, 0, 2 * tf, 94, 104, 100), 500);
    // Rows outside every source (above the book) never block a tick.
    EXPECT_TRUE(buildsInView(s, 100, 0, 2 * tf, 99, 100) && buildsInView(s, 500, 0, 2 * tf, 190, 260));
    EXPECT_EQ(autoTickUnits(s, 0, 0, 2 * tf, 250, 258, 100), 100);
    // Same slice-T hysteresis: 2 -> 1 only when the finer row reaches 2.5 px.
    EXPECT_EQ(autoTickUnits(s, 200, 0, 2 * tf, 96, 104, 19), 200);
    EXPECT_EQ(autoTickUnits(s, 200, 0, 2 * tf, 96, 104, 20), 100);
    // No loaded column in view: nothing to pick.
    EXPECT_EQ(autoTickUnits(s, 0, 5 * tf, 6 * tf, 96, 104, 100), 0);
}
TEST(HeatmapSpanPlanner, ManualOffersEveryBuildablePresetAndNamesVeiledRanges) {
    const auto fine = data(100, 9500, 10500), coarse = data(500, 0, 20000);
    const auto s = summary(fine, coarse);
    EXPECT_EQ(manualPresetUnits(s, 1000), (std::vector<int64_t>{100, 200, 500, 1000}));
    // $1 locked over rows beyond the band: whole absolute rows, including the
    // row the view edge crosses; consecutive columns merge into one range.
    const auto veil = veiledRanges(s, 100, 0, 2 * tf, 94.5, 105.2);
    ASSERT_EQ(veil.size(), 2u);
    EXPECT_EQ(veil[0], (VeiledRange{0, 2 * tf, {9400, 9500}}));
    EXPECT_EQ(veil[1], (VeiledRange{0, 2 * tf, {10500, 10600}}));
    EXPECT_TRUE(veiledRanges(s, 500, 0, 2 * tf, 94.5, 105.2).empty());
    EXPECT_TRUE(veiledRanges(s, 100, 0, 2 * tf, 96, 104).empty());
}
TEST(HeatmapSpanPlanner, CoverageHolesCannotClaimTheFineBand) {
    auto fine = data(100, 9500, 10500);
    const auto coarse = data(500, 0, 20000);
    // Column 1 lost ask coverage of row $100: the hole is covered by the coarse
    // source only, so $1 veils there and Auto cannot pick it.
    fine.columns[1].native[0].coverage[1] = {{95, 99, tf}, {101, 104, tf}};
    const auto holed = summary(fine, coarse);
    EXPECT_EQ(autoTickUnits(holed, 100, 0, 2 * tf, 96, 104, 100), 500);
    const auto veil = veiledRanges(holed, 100, 0, 2 * tf, 96, 104);
    ASSERT_EQ(veil.size(), 1u);
    EXPECT_EQ(veil.front(), (VeiledRange{tf, 2 * tf, {10000, 10100}}));
    // A partially covered run is not full coverage either.
    auto partial = data(100, 9500, 10500);
    partial.columns[0].native[0].coverage[0][0].coveredMs = tf / 2;
    EXPECT_TRUE(summarizeResolution(partial, "p", tf, 0, 2 * tf).columns[0].sources[0].bands.empty());
}
// A grid change inside a column: two constituents that share the column's
// observed time. The summary's buildable rows must equal binColumn's validity
// (the binning oracle) at every tick, including a partially covered run.
TEST(HeatmapSpanPlanner, GridChangeSummaryMatchesTheBinningOracle) {
    SparseColumns data{"BTC-USD", "near", tf, 0, tf, {}, {}};
    data.scannedRanges = {{0, tf}};
    NativeColumn fine;
    fine.grid = {1, 100, 100};
    fine.observedMs = tf / 2;
    fine.coverage[0] = fine.coverage[1] = {{95, 104, uint64_t(tf / 2)}};
    NativeColumn changed;
    changed.grid = {2, 250, 100};
    changed.observedMs = tf / 2;
    // Rows 38-39 ($95-$100) fully covered; 40-41 ($100-$105) only a quarter.
    changed.coverage[0] = changed.coverage[1] = {{38, 39, uint64_t(tf / 2)}, {40, 41, uint64_t(tf / 4)}};
    data.columns.push_back({0, uint64_t(tf), 0, {fine, changed}});
    ASSERT_NO_THROW(validate(data));
    const auto summary = summarizeResolution(data, "any", tf, 0, tf);
    const auto &source = summary.columns.at(0).sources.at(0);
    EXPECT_EQ(source.nativeTicks, (std::vector<int64_t>{100, 250}));
    EXPECT_EQ(source.commonUnits, 500);
    EXPECT_EQ(source.bands, (std::vector<PriceRange>{{9500, 10000}}));
    for (const int64_t tick : {100, 200, 250, 500, 1000, 2500}) {
        const auto cells = binColumn(data.columns[0], 90, 110, double(tick) / 100);
        const int64_t first = 9000 / tick * tick;
        for (size_t row = 0; row < cells.size(); ++row) {
            const int64_t lo = first + int64_t(row) * tick;
            const bool oracle = cells[cells.size() - 1 - row].valid;
            bool inBand = false;
            for (const auto &band : source.bands) inBand = inBand || (band.lo <= lo && lo + tick <= band.end);
            EXPECT_EQ(buildsOn(tick, source.commonUnits) && inBand, oracle) << "tick=" << tick << " lo=" << lo;
            // Single source: rows it covers but cannot build are exactly the veil.
            if (inBand)
                EXPECT_EQ(unbuildableRows(summary.columns[0], tick, {lo, lo + tick}).empty(), oracle) << tick;
        }
    }
}

// The tick-eligibility algorithm as it was before the reusable scratch buffers
// (HeatmapSpanPlanner.cpp at 800d29a7): a fresh vector per step. The function bodies
// are that code with three edits: calls qualified with reference:: or recording::,
// the Ranges alias declared here, and inView inlined into buildsInView. The scratch
// path must choose exactly what it chose for every input.
namespace reference {
using Ranges = std::vector<PriceRange>;
Ranges unite(Ranges ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.lo < b.lo; });
    Ranges out;
    for (const auto &r : ranges) {
        if (r.end <= r.lo) continue;
        if (!out.empty() && r.lo <= out.back().end) out.back().end = std::max(out.back().end, r.end);
        else out.push_back(r);
    }
    return out;
}
Ranges intersect(const Ranges &a, const Ranges &b) {
    Ranges out;
    for (size_t i = 0, j = 0; i < a.size() && j < b.size();) {
        const auto lo = std::max(a[i].lo, b[j].lo), end = std::min(a[i].end, b[j].end);
        if (lo < end) out.push_back({lo, end});
        if (a[i].end < b[j].end) ++i;
        else ++j;
    }
    return out;
}
Ranges subtract(const Ranges &a, const Ranges &b) {
    Ranges out;
    size_t j = 0;
    for (auto r : a) {
        while (j < b.size() && b[j].end <= r.lo) ++j;
        for (size_t k = j; k < b.size() && b[k].lo < r.end; ++k) {
            if (b[k].lo > r.lo) out.push_back({r.lo, b[k].lo});
            r.lo = std::max(r.lo, b[k].end);
        }
        if (r.lo < r.end) out.push_back(r);
    }
    return out;
}
Ranges inward(const Ranges &ranges, int64_t tick) {
    Ranges out;
    for (const auto &r : ranges) {
        const int64_t lo = -recording::floorDiv(-r.lo, tick) * tick, end = recording::floorDiv(r.end, tick) * tick;
        if (lo < end) out.push_back({lo, end});
    }
    return out;
}
std::optional<PriceRange> binsInView(double priceLo, double priceHi, double scale, int64_t tick) {
    if (tick <= 0 || !std::isfinite(priceLo) || !std::isfinite(priceHi) || !(priceHi > priceLo)) return std::nullopt;
    const double lo = std::floor(std::max(priceLo, 0.0) * scale), hi = std::ceil(priceHi * scale);
    if (!(hi > lo) || hi >= 0x1p62) return std::nullopt;
    const int64_t first = recording::floorDiv(int64_t(lo), tick), last = recording::floorDiv(int64_t(hi) - 1, tick);
    return PriceRange{first * tick, (last + 1) * tick};
}
Ranges unbuildableRows(const ColumnResolution &column, int64_t tick, PriceRange rows) {
    if (tick <= 0 || rows.end <= rows.lo) return {};
    Ranges data, covered;
    for (const auto &source : column.sources) {
        if (source.state != BucketState::Present) continue;
        data.insert(data.end(), source.bands.begin(), source.bands.end());
        if (buildsOn(tick, source.commonUnits)) {
            const auto whole = inward(source.bands, tick);
            covered.insert(covered.end(), whole.begin(), whole.end());
        }
    }
    const auto needed = intersect(inward(unite(std::move(data)), tick), {rows});
    return subtract(needed, unite(std::move(covered)));
}
bool buildsInView(const ResolutionSummary &summary, int64_t tick, double timeLo, double timeHi, double priceLo,
                  double priceHi) {
    const auto rows = binsInView(priceLo, priceHi, summary.priceScale, tick);
    if (!rows) return false;
    bool loaded = false;
    for (const auto &column : summary.columns) {
        if (!(double(column.startMs) < timeHi && double(column.startMs + summary.tfMs) > timeLo)) continue;
        for (const auto &source : column.sources)
            if (source.state == BucketState::Present && buildsOn(tick, source.commonUnits)) loaded = true;
        if (!reference::unbuildableRows(column, tick, *rows).empty()) return false;
    }
    return loaded;
}
int64_t autoTickUnits(const ResolutionSummary &summary, int64_t currentUnits, double timeLo, double timeHi,
                      double priceLo, double priceHi, double heightPx, const AutoTickParams &params) {
    if (!std::isfinite(heightPx) || !(heightPx > 0) || !(priceHi > priceLo) || summary.columns.empty()) return 0;
    const double unitsPerPx = (priceHi - priceLo) * summary.priceScale / heightPx;
    return autoTickUnitsIf(currentUnits, unitsPerPx, [&](int64_t tick) {
        return reference::buildsInView(summary, tick, timeLo, timeHi, priceLo, priceHi);
    }, params);
}
} // namespace reference

// Seeded random summaries (overlapping, unsorted, empty and touching bands; gap and
// not-loaded sources; mixed common ticks) and views. One scratch serves every call,
// so a buffer that keeps stale rows between calls shows up as a different answer.
TEST(HeatmapSpanPlanner, ScratchTickChoiceMatchesTheAllocatingAlgorithm) {
    std::mt19937_64 rng(20261118);
    auto uniform = [&](int64_t lo, int64_t hi) { return std::uniform_int_distribution<int64_t>(lo, hi)(rng); };
    auto real = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    const int64_t commons[] = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 100, 1000};
    TickScratch scratch;
    std::set<int64_t> autoTicks;
    size_t builds = 0, refusals = 0, rowsCompared = 0, veils = 0;
    for (int trial = 0; trial < 1500; ++trial) {
        ResolutionSummary summary{tf, trial % 5 ? 100.0 : 1000.0, {}};
        const int64_t columns = uniform(1, 60);
        const int64_t centre = uniform(5'000, 15'000) * 100;
        for (int64_t c = 0; c < columns; ++c) {
            ColumnResolution column{c * tf, {}};
            for (int64_t s = uniform(0, 3); s > 0; --s) {
                SourceResolution source;
                const int64_t state = uniform(0, 9);
                source.state = state == 0 ? BucketState::Gap : state == 1 ? BucketState::NotLoaded : BucketState::Present;
                source.commonUnits = uniform(0, 30) ? commons[uniform(0, 12)] : 0;
                for (int64_t b = uniform(0, 4); b > 0; --b) {
                    const int64_t lo = centre + uniform(-60'000, 60'000);
                    source.bands.push_back({lo, lo + uniform(-500, 50'000)});
                }
                if (uniform(0, 1)) std::sort(source.bands.begin(), source.bands.end(),
                                             [](const auto &a, const auto &b) { return a.lo < b.lo; });
                column.sources.push_back(std::move(source));
            }
            summary.columns.push_back(std::move(column));
        }
        for (int view = 0; view < 12; ++view) {
            const double scale = summary.priceScale;
            const double priceLo = double(centre) / scale + real(-800, 800), priceHi = priceLo + real(-5, 1500);
            const double timeLo = real(-2, double(columns)) * double(tf), timeHi = timeLo + real(0, 70) * double(tf);
            const double height = view % 6 ? real(40, 2400) : (view % 4 ? 0.0 : std::nan(""));
            const auto &ladder = presetLadderUnits();
            const int64_t current = uniform(0, 3) ? ladder[size_t(uniform(0, 20))] : uniform(0, 777);
            const AutoTickParams params{real(0.5, 6), real(-0.1, 0.95)};
            const auto expected = reference::autoTickUnits(summary, current, timeLo, timeHi, priceLo, priceHi, height,
                                                           params);
            ASSERT_EQ(autoTickUnits(summary, current, timeLo, timeHi, priceLo, priceHi, height, params, scratch),
                      expected) << "trial " << trial << " view " << view;
            ASSERT_EQ(autoTickUnits(summary, current, timeLo, timeHi, priceLo, priceHi, height, params), expected);
            autoTicks.insert(expected);
            for (const int64_t tick : {ladder[size_t(uniform(0, 20))], current, expected}) {
                const bool want = reference::buildsInView(summary, tick, timeLo, timeHi, priceLo, priceHi);
                ASSERT_EQ(buildsInView(summary, tick, timeLo, timeHi, priceLo, priceHi, scratch), want);
                ASSERT_EQ(buildsInView(summary, tick, timeLo, timeHi, priceLo, priceHi), want);
                (want ? builds : refusals)++;
                if (const auto rows = reference::binsInView(priceLo, priceHi, scale, tick))
                    for (const auto &column : summary.columns) {
                        const auto rowsWant = reference::unbuildableRows(column, tick, *rows);
                        ASSERT_EQ(unbuildableRows(column, tick, *rows, scratch), rowsWant);
                        ASSERT_EQ(unbuildableRows(column, tick, *rows), rowsWant);
                        ++rowsCompared;
                        veils += rowsWant.size();
                    }
            }
        }
    }
    // The inputs reach every branch: many ticks, both answers, non-empty veils.
    EXPECT_GE(autoTicks.size(), 15u);
    EXPECT_GT(builds, 1000u);
    EXPECT_GT(refusals, 1000u);
    EXPECT_GT(veils, 1000u);
    std::printf("differential: %zu auto ticks distinct, %zu builds, %zu refusals, %zu column rows compared\n",
                autoTicks.size(), builds, refusals, rowsCompared);
}
} // namespace
