#include <gtest/gtest.h>

#include "render/HeatmapColumnWindow.hpp"

#include <QtEndian>
#include <algorithm>
#include <cstring>

using namespace heatmap_window;

namespace {

constexpr int64_t kTf = 60'000;
constexpr int kRows = 16;
constexpr int kWidth = 8;
constexpr int64_t kBase = 1'790'000'000'000 / kTf * kTf;

int64_t bucket(int i) { return kBase + i * kTf; }

uint16_t cell(const QByteArray& bytes, int row) {
    uint16_t v = 0;
    std::memcpy(&v, bytes.constData() + row * 2, 2);
    return qFromLittleEndian(v);
}

// Band [lo, lo + rows) at tick 1; one bid cell at `row` holding `value`.
Column makeColumn(int i, double lo = 100.0, int row = 3, uint16_t value = 1000) {
    Column c;
    c.bucketStartMs = bucket(i);
    c.minPrice = lo;
    c.maxPrice = lo + kRows;
    c.tickSize = 1.0;
    c.intensity = QByteArray(kRows * 2, 0);
    const uint16_t le = qToLittleEndian(value);
    std::memcpy(c.intensity.data() + row * 2, &le, 2);
    c.liquidity = QByteArray(kRows * 2, 0);
    return c;
}

ColumnWindow makeWindow(int width = kWidth, int capacity = 0) {
    ColumnWindow w;
    w.configure(kTf, width, capacity);
    return w;
}

bool live(ColumnWindow& w, const Column& c, Update& out) {
    bool first = false;
    return w.ingestLive(c, 2, out, first);
}

const SlotWrite* writeFor(const Update& u, int64_t bucketMs) {
    const auto it = std::find_if(u.writes.begin(), u.writes.end(),
                                 [bucketMs](const SlotWrite& s) { return s.bucketStartMs == bucketMs; });
    return it == u.writes.end() ? nullptr : &*it;
}

int coverageAt(const Update& u, int64_t bucketMs) {
    const int64_t start = u.windowEndMs - (u.width - 1) * u.timeframeMs;
    return u.coverage.at(static_cast<int>((bucketMs - start) / u.timeframeMs));
}

} // namespace

TEST(HeatmapColumnWindow, FirstLiveColumnPlacesPinnedWindow) {
    auto w = makeWindow();
    Update u;
    bool first = false;
    ASSERT_TRUE(w.ingestLive(makeColumn(0), 2, u, first));
    EXPECT_TRUE(first);
    EXPECT_TRUE(u.full);
    EXPECT_TRUE(u.pinnedToLive);
    EXPECT_EQ(u.windowEndMs, bucket(0));
    EXPECT_EQ(u.liveBucketMs, bucket(0));
    EXPECT_EQ(static_cast<int>(u.writes.size()), kWidth);
    EXPECT_EQ(coverageAt(u, bucket(0)), 1);
    EXPECT_EQ(coverageAt(u, bucket(-1)), 0);
}

TEST(HeatmapColumnWindow, LiveSlidesByRewritingOnlyTheEnteringSlot) {
    auto w = makeWindow();
    Update u;
    for (int i = 0; i < 3; ++i) live(w, makeColumn(i), u);
    ASSERT_TRUE(live(w, makeColumn(3), u));
    EXPECT_FALSE(u.full);
    ASSERT_EQ(u.writes.size(), 1u);
    EXPECT_EQ(u.writes[0].bucketStartMs, bucket(3));
    EXPECT_EQ(u.newestSlot, u.writes[0].slot);
    EXPECT_EQ(u.windowEndMs, bucket(3));

    // Forming update of the same bucket rewrites that slot only.
    ASSERT_TRUE(live(w, makeColumn(3, 100.0, 3, 2000), u));
    ASSERT_EQ(u.writes.size(), 1u);
    EXPECT_EQ(cell(u.writes[0].intensity, 3), 2000);
}

TEST(HeatmapColumnWindow, SkippedLiveBucketStaysMissingInsteadOfCopied) {
    auto w = makeWindow();
    Update u;
    live(w, makeColumn(0), u);
    ASSERT_TRUE(live(w, makeColumn(2), u));
    const SlotWrite* gap = writeFor(u, bucket(1));
    ASSERT_NE(gap, nullptr);
    EXPECT_FALSE(gap->recorded);
    EXPECT_EQ(cell(gap->intensity, 3), 0);
    EXPECT_EQ(coverageAt(u, bucket(1)), 0);
    EXPECT_EQ(coverageAt(u, bucket(2)), 1);
}

TEST(HeatmapColumnWindow, LiveWhilePannedAwayIsCachedAndShownOnReturn) {
    auto w = makeWindow();
    Update u;
    for (int i = 0; i <= 20; ++i) live(w, makeColumn(i), u);

    // Pan back to buckets 5..7 with follow off: the window leaves the live edge.
    ASSERT_TRUE(w.setViewport(bucket(5), bucket(7), false, u));
    EXPECT_FALSE(u.pinnedToLive);
    EXPECT_LT(u.windowEndMs, bucket(20));

    // Live keeps arriving; nothing is uploaded for the far-away window.
    EXPECT_FALSE(live(w, makeColumn(21), u));
    EXPECT_FALSE(live(w, makeColumn(22), u));

    // Following again pins to the live edge and shows the columns that arrived meanwhile.
    ASSERT_TRUE(w.setViewport(bucket(15), bucket(22), true, u));
    EXPECT_TRUE(u.pinnedToLive);
    EXPECT_EQ(u.windowEndMs, bucket(22));
    for (int i : {21, 22}) {
        const SlotWrite* s = writeFor(u, bucket(i));
        ASSERT_NE(s, nullptr) << i;
        EXPECT_TRUE(s->recorded);
        EXPECT_EQ(coverageAt(u, bucket(i)), 1);
    }
}

TEST(HeatmapColumnWindow, ManualViewNearLiveStopsSlidingBeforeItFallsOff) {
    auto w = makeWindow();
    Update u;
    live(w, makeColumn(0), u);
    w.setViewport(bucket(0), bucket(2), false, u);  // at the live edge, not following
    int slides = 0;
    for (int i = 1; i <= 20; ++i) {
        if (live(w, makeColumn(i), u)) ++slides;
    }
    // The window may slide while bucket 0 stays inside it (7 more buckets), then holds.
    EXPECT_LE(slides, kWidth - 1);
    EXPECT_LE(w.windowEndMs(), bucket(kWidth - 1));
}

TEST(HeatmapColumnWindow, HistoryFillsOlderBucketsAndKnownRangesStopRefetch) {
    auto w = makeWindow(64);
    Update u;
    for (int i = 100; i <= 105; ++i) live(w, makeColumn(i), u);
    w.setViewport(bucket(80), bucket(105), true, u);

    FetchRequest req;
    ASSERT_TRUE(w.nextFetch(req));
    EXPECT_EQ(req.endMs, bucket(99));

    std::vector<Column> page;
    for (int i = 90; i <= 99; ++i) {
        if (i != 95) page.push_back(makeColumn(i));
    }
    bool first = false;
    ASSERT_TRUE(w.ingestHistory(page, 2, req.endMs, req.count, bucket(90), u, first));
    EXPECT_FALSE(first);
    EXPECT_EQ(coverageAt(u, bucket(94)), 1);
    EXPECT_EQ(coverageAt(u, bucket(95)), 0);  // known missing, not refetched
    EXPECT_FALSE(w.nextFetch(req));
    EXPECT_EQ(w.oldestAvailableMs(), bucket(90));
}

TEST(HeatmapColumnWindow, FullHistoryPageOnlyMarksItsOwnRangeKnown) {
    auto w = makeWindow(64);
    Update u;
    live(w, makeColumn(200), u);
    w.setViewport(bucket(150), bucket(200), true, u);
    std::vector<Column> page = {makeColumn(198), makeColumn(199)};
    bool first = false;
    w.ingestHistory(page, 2, bucket(199), static_cast<int>(page.size()), 0, u, first);
    FetchRequest req;
    ASSERT_TRUE(w.nextFetch(req));
    EXPECT_EQ(req.endMs, bucket(197));
}

TEST(HeatmapColumnWindow, BandChangeWhilePinnedRebuildsIntoTheNewBand) {
    auto w = makeWindow();
    Update u;
    live(w, makeColumn(0, 100.0, 3), u);  // row 3 = price 112.5
    ASSERT_TRUE(live(w, makeColumn(1, 104.0, 3), u));
    EXPECT_TRUE(u.full);
    EXPECT_DOUBLE_EQ(u.band.minPrice, 104.0);
    // Bucket 0's cell at 112.5 lands on row 7 of the [104, 120) band.
    const SlotWrite* older = writeFor(u, bucket(0));
    ASSERT_NE(older, nullptr);
    EXPECT_EQ(cell(older->intensity, 7), 1000);
    EXPECT_EQ(cell(older->intensity, 3), 0);
}

TEST(HeatmapColumnWindow, PannedWindowKeepsItsBandWhenLiveRecenters) {
    auto w = makeWindow();
    Update u;
    for (int i = 0; i <= 20; ++i) live(w, makeColumn(i), u);
    w.setViewport(bucket(2), bucket(4), false, u);
    const double bandMin = u.band.minPrice;
    EXPECT_FALSE(live(w, makeColumn(21, 150.0), u));
    Update again;
    EXPECT_FALSE(w.setViewport(bucket(2), bucket(4), false, again));
    EXPECT_DOUBLE_EQ(bandMin, 100.0);
}

TEST(HeatmapColumnWindow, EvictionNeverDropsWindowColumns) {
    auto w = makeWindow(kWidth, 0);  // capacity clamps to width + one page
    Update u;
    const int total = kWidth + ColumnWindow::kPageColumns + 50;
    for (int i = 0; i < total; ++i) live(w, makeColumn(i), u);
    EXPECT_LE(static_cast<int>(w.cachedColumns()), kWidth + ColumnWindow::kPageColumns);
    EXPECT_EQ(u.windowEndMs, bucket(total - 1));
    for (int i = total - kWidth; i < total; ++i) {
        EXPECT_EQ(coverageAt(u, bucket(i)), 1) << i;
    }
}

TEST(HeatmapColumnWindowResample, MergedRowsKeepTheStrongerBidAndAsk) {
    // Two source rows collapse into one target row (target tick 2x source).
    Column src = makeColumn(0, 100.0, 4, 30000);
    const uint16_t weakBid = qToLittleEndian<uint16_t>(100);
    std::memcpy(src.intensity.data() + 5 * 2, &weakBid, 2);
    const Band target{100.0, 132.0, 2.0};
    QByteArray intensity, liquidity;
    resampleColumn(src, target, kRows, 2, intensity, liquidity);
    // Rows 4 and 5 (prices 111.5, 110.5) map to target row 10 of [100,132) at tick 2.
    EXPECT_EQ(cell(intensity, 10), 30000);

    EXPECT_EQ(intensityMagnitude(0x8000 + 500, 2), 500);
    EXPECT_EQ(intensityMagnitude(700, 2), 700);
    EXPECT_GT(intensityMagnitude(30000, 2), intensityMagnitude(100, 2));
}

TEST(HeatmapColumnWindowResample, SameBandSharesBytes) {
    const Column src = makeColumn(0);
    QByteArray intensity, liquidity;
    resampleColumn(src, Band{src.minPrice, src.maxPrice, src.tickSize}, kRows, 2, intensity, liquidity);
    EXPECT_EQ(intensity.constData(), src.intensity.constData());
}

TEST(HeatmapColumnWindow, RecordedZeroColumnCountsAsRecorded) {
    auto w = makeWindow();
    Update u;
    Column zero = makeColumn(0);
    zero.intensity.fill(0);
    ASSERT_TRUE(live(w, zero, u));
    EXPECT_EQ(coverageAt(u, bucket(0)), 1);  // INV-047: zero liquidity is data, not a gap
}

#include "render/HeatmapStreamState.hpp"

TEST(HeatmapStreamStateWindow, ApplyWindowMovesCursorAndQueuesOnlyGivenSlots) {
    HeatmapStreamState state;
    HeatmapStreamState::WindowPlacement placement;
    placement.timeframeMs = kTf;
    placement.gridWidth = kWidth;
    placement.gridHeight = kRows;
    placement.bytesPerCell = 2;
    placement.minPrice = 100.0;
    placement.maxPrice = 116.0;
    placement.tickSize = 1.0;
    placement.windowEndMs = bucket(10);
    placement.newestSlot = 2;
    placement.full = true;

    std::vector<HeatmapStreamState::SlotColumn> slotColumns;
    for (int x = 0; x < kWidth; ++x) {
        slotColumns.push_back({x, makeColumn(x).intensity, {}, 1.0});
    }
    ASSERT_TRUE(state.applyWindow(placement, std::move(slotColumns), 1000));
    state.updateTimeOffset(0.0f);
    auto snap = state.snapshot();
    EXPECT_EQ(snap.lastSliceStartMs, bucket(10));
    EXPECT_EQ(snap.timeOriginMs, bucket(10) - (kWidth - 1) * kTf);
    EXPECT_EQ(snap.filledColumns, kWidth);
    EXPECT_EQ(state.writeColumn(), 2);
    EXPECT_FLOAT_EQ(snap.timeOffset, 3.0f / kWidth);  // oldest slot = newest + 1
    EXPECT_EQ(state.pendingUploadCount(), kWidth);

    // Incremental: one entering slot, cursor advances, only that slot queued.
    std::vector<HeatmapStreamState::PendingColumn> pending;
    state.takePendingUploads(pending);
    placement.full = false;
    placement.windowEndMs = bucket(11);
    placement.newestSlot = 3;
    std::vector<HeatmapStreamState::SlotColumn> one{{3, makeColumn(11).intensity, {}, 1.0}};
    ASSERT_TRUE(state.applyWindow(placement, std::move(one), 2000));
    EXPECT_EQ(state.pendingUploadCount(), 1);
    EXPECT_EQ(state.writeColumn(), 3);

    // Wrong-sized slot data is rejected without touching state.
    std::vector<HeatmapStreamState::SlotColumn> bad{{0, QByteArray(3, 0), {}, 1.0}};
    EXPECT_FALSE(state.applyWindow(placement, std::move(bad), 3000));
}
