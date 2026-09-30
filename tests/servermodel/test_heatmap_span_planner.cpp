#include "heatmap/HeatmapSpanPlanner.hpp"
#include <gtest/gtest.h>
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
        EXPECT_EQ(source.chunks.front().levelMs, descriptor->hourLevel ? kHourMs : kMinuteMs);
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
TEST(HeatmapSpanPlanner, CoverageHolesAndGridChangesCannotClaimTheFineBand) {
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
    // A grid change inside the column: its common tick is the LCM.
    auto changed = fine.columns[0].native[0];
    changed.grid.rowTickUnits = 250;
    changed.baseRow = 0;
    changed.coverage[0] = changed.coverage[1] = {{38, 41, tf}};
    fine.columns[0].native.push_back(changed);
    const auto mixed = summarizeResolution(fine, "fine", tf, 0, 2 * tf);
    EXPECT_EQ(mixed.columns[0].sources[0].commonUnits, 500);
    EXPECT_EQ(mixed.columns[0].sources[0].nativeTicks, (std::vector<int64_t>{100, 250}));
    EXPECT_EQ(mixed.columns[0].sources[0].bands, (std::vector<PriceRange>{{9500, 10500}}));
}
} // namespace
