#include <gtest/gtest.h>

#include "render/HeatmapHistoryWindow.hpp"
#include "render/HeatmapStreamState.hpp"

#include <QtEndian>

namespace {

using Column = IGridDataSource::HeatmapHistoryColumn;

Column column(int64_t startMs,
              double minPrice,
              double maxPrice,
              double tickSize,
              std::initializer_list<uint16_t> values) {
    Column out;
    out.bucketStartMs = startMs;
    out.bucketEndMs = startMs + 60'000;
    out.minPrice = minPrice;
    out.maxPrice = maxPrice;
    out.tickSize = tickSize;
    out.intensity.resize(static_cast<int>(values.size() * sizeof(uint16_t)));
    auto* dest = reinterpret_cast<uint16_t*>(out.intensity.data());
    int i = 0;
    for (uint16_t value : values) {
        dest[i++] = qToLittleEndian(value);
    }
    return out;
}

bool hasNonZero(const QByteArray& bytes) {
    const auto* values = reinterpret_cast<const uint16_t*>(bytes.constData());
    const int count = bytes.size() / static_cast<int>(sizeof(uint16_t));
    for (int i = 0; i < count; ++i) {
        if (qFromLittleEndian(values[i]) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(HeatmapHistoryWindow, RepresentsMissingBucketsAsBlankColumns) {
    QVector<Column> source{
        column(60'000, 0.0, 4.0, 1.0, {100, 200, 300, 400}),
        column(180'000, 0.0, 4.0, 1.0, {500, 600, 700, 800}),
    };

    heatmap_history::Window window;
    ASSERT_TRUE(heatmap_history::buildWindow(source, 60'000, 3, 4, 0, window));
    ASSERT_EQ(window.columns.size(), 3);
    EXPECT_EQ(window.columns[0].bucketStartMs, 60'000);
    EXPECT_EQ(window.columns[1].bucketStartMs, 120'000);
    EXPECT_EQ(window.columns[2].bucketStartMs, 180'000);
    ASSERT_EQ(window.coverage.size(), 3);
    EXPECT_EQ(static_cast<int>(window.coverage[0]), 1);
    EXPECT_EQ(static_cast<int>(window.coverage[1]), 0);
    EXPECT_EQ(static_cast<int>(window.coverage[2]), 1);
    EXPECT_TRUE(hasNonZero(window.columns[0].intensity));
    EXPECT_FALSE(hasNonZero(window.columns[1].intensity));
    EXPECT_TRUE(hasNonZero(window.columns[2].intensity));
}

TEST(HeatmapHistoryWindow, ResamplesRecenteredBandsIntoOnePriceRange) {
    QVector<Column> source{
        column(60'000, 0.0, 4.0, 1.0, {100, 200, 300, 400}),
        column(120'000, 4.0, 8.0, 1.0, {500, 600, 700, 800}),
    };

    heatmap_history::Window window;
    ASSERT_TRUE(heatmap_history::buildWindow(source, 60'000, 2, 4, 0, window));
    EXPECT_DOUBLE_EQ(window.minPrice, 0.0);
    EXPECT_DOUBLE_EQ(window.maxPrice, 8.0);
    EXPECT_DOUBLE_EQ(window.tickSize, 2.0);
    EXPECT_TRUE(hasNonZero(window.columns[0].intensity));
    EXPECT_TRUE(hasNonZero(window.columns[1].intensity));
    for (const auto& result : window.columns) {
        EXPECT_DOUBLE_EQ(result.minPrice, window.minPrice);
        EXPECT_DOUBLE_EQ(result.maxPrice, window.maxPrice);
        EXPECT_DOUBLE_EQ(result.tickSize, window.tickSize);
    }
}

TEST(HeatmapHistoryWindow, KeepsLatestBucketsWithinGpuCapacity) {
    QVector<Column> source;
    for (int i = 1; i <= 5; ++i) {
        source.push_back(column(i * 60'000, 0.0, 4.0, 1.0, {100, 200, 300, 400}));
    }

    heatmap_history::Window window;
    ASSERT_TRUE(heatmap_history::buildWindow(source, 60'000, 3, 4, 0, window));
    ASSERT_EQ(window.columns.size(), 3);
    EXPECT_EQ(window.columns.front().bucketStartMs, 180'000);
    EXPECT_EQ(window.columns.back().bucketStartMs, 300'000);
}

TEST(HeatmapHistoryWindow, AnchorsBlankOutageAtRequestedEnd) {
    QVector<Column> source{
        column(60'000, 0.0, 4.0, 1.0, {100, 200, 300, 400}),
    };

    heatmap_history::Window window;
    ASSERT_TRUE(heatmap_history::buildWindow(source, 60'000, 3, 4, 600'000, window));
    ASSERT_EQ(window.columns.size(), 3);
    EXPECT_EQ(window.columns.front().bucketStartMs, 480'000);
    EXPECT_EQ(window.columns.back().bucketStartMs, 600'000);
    for (const auto& result : window.columns) {
        EXPECT_FALSE(hasNonZero(result.intensity));
    }
    EXPECT_EQ(window.coverage, QByteArray(3, 0));
}

TEST(HeatmapHistoryWindow, MarksRecordedZeroColumnAsCovered) {
    QVector<Column> source{
        column(60'000, 0.0, 4.0, 1.0, {0, 0, 0, 0}),
    };

    heatmap_history::Window window;
    ASSERT_TRUE(heatmap_history::buildWindow(source, 60'000, 1, 4, 0, window));
    ASSERT_EQ(window.coverage.size(), 1);
    EXPECT_EQ(static_cast<int>(window.coverage[0]), 1);
    EXPECT_FALSE(hasNonZero(window.columns[0].intensity));
}

TEST(HeatmapStreamState, ReplacesHistoricalWindowInLinearSizedBatch) {
    HeatmapStreamState state;
    state.reset(3, 2, 0.0, 4.0, 2.0);
    state.setIntensityBytesPerCell(2);

    std::vector<HeatmapStreamState::WindowColumn> columns;
    for (int i = 1; i <= 3; ++i) {
        QByteArray intensity(2 * static_cast<int>(sizeof(uint16_t)), 0);
        auto* values = reinterpret_cast<uint16_t*>(intensity.data());
        values[0] = qToLittleEndian(static_cast<uint16_t>(i * 100));
        values[1] = qToLittleEndian(static_cast<uint16_t>(i * 200));
        columns.push_back({i * 60'000, intensity, {}, 1.0});
    }

    ASSERT_TRUE(state.replaceWindow(60'000, columns, 500));
    const auto snapshot = state.snapshot();
    EXPECT_EQ(snapshot.filledColumns, 3);
    EXPECT_EQ(snapshot.lastSliceStartMs, 180'000);
    EXPECT_EQ(snapshot.timeOriginMs, 60'000);
    EXPECT_EQ(state.pendingUploadCount(), 3);
    EXPECT_EQ(state.writeColumn(), 0);
}

TEST(HeatmapStreamState, BoundsVeryLargeLiveGapByGridWidth) {
    HeatmapStreamState state;
    state.reset(4, 2, 0.0, 4.0, 2.0);
    state.setIntensityBytesPerCell(1);
    const QByteArray intensity(2, static_cast<char>(42));

    state.ingestSlice(60'000, 60'000, intensity, {}, 1.0, 0);
    state.ingestSlice(6'000'000'000, 60'000, intensity, {}, 1.0, 1);

    EXPECT_EQ(state.snapshot().filledColumns, 4);
    EXPECT_LE(state.pendingUploadCount(), 4);
}
