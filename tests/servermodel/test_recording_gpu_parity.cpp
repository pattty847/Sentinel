#include "lab/GpuBinner.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QDateTime>
#include <QTemporaryDir>
#include <QtGlobal>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif

namespace {
constexpr int64_t minute = 60'000;
constexpr int64_t hour = 60 * minute;

TEST(LabGridPlacement, FractionalPansStayInsideAbsoluteBinMargin) {
    lab::Grid grid;
    grid.firstMinute = 10; // absolute minute 30010, a multiple of five
    grid.timeframeMinutes = 5;
    grid.columns = 9;
    grid.baseRow = 20'000;
    grid.rowLo = 80'000; // absolute native row 100000, display bin 10000
    grid.group = 10;
    grid.rows = 10;
    constexpr int64_t sourceStartMinute = 30'000;
    constexpr double tick = 10;
    EXPECT_TRUE(lab::gridContainsView(grid, sourceStartMinute, grid.baseRow,
                                      30'020.2, 30'040.2, 100'020.2, 100'070.2, tick));
    EXPECT_TRUE(lab::gridContainsView(grid, sourceStartMinute, grid.baseRow,
                                      30'020.8, 30'040.8, 100'020.8, 100'070.8, tick));
    EXPECT_FALSE(lab::gridContainsView(grid, sourceStartMinute, grid.baseRow,
                                       30'030.2, 30'050.2, 100'020.2, 100'070.2, tick));
    EXPECT_FALSE(lab::gridContainsView(grid, sourceStartMinute, grid.baseRow,
                                       30'020.2, 30'040.2, 100'040.2, 100'090.2, tick));
}

void writeFixture(const std::filesystem::path &root) {
    recording::Hmc2Store writer(root);
    recording::Hmc2Record r;
    r.header = {"BTC-USD", "deep", minute, 100, 1000, {}, 42};
    r.observedMs = minute;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'050;
    for (int c = 0; c < 1440; ++c) {
        if (c % 11 == 3) continue;
        const bool coarse = c < 720;
        r.header.rowTickUnits = coarse ? 1000 : 500;
        r.header.configHash = coarse ? 42 : 43;
        r.header.sizeScale.floor = coarse ? 1e-6 : 1e-8;
        const int base = coarse ? 10'000 : 20'000;
        r.bucketStartMs = recording::kHmc2MinMs + int64_t(c) * minute;
        r.bidRowLo = base;
        r.bidRowHi = base + (c % 7 == 0 ? 3 : (coarse ? 5 : 11));
        r.askRowLo = base + (c % 13 == 0 ? 2 : 0);
        r.askRowHi = base + (coarse ? 5 : 11);
        r.entries = {{base + 1, false, recording::encodeSize(10 + c % 5, r.header.sizeScale), 0},
                     {base + 2, true, recording::encodeSize(7 + c % 3, r.header.sizeScale), 0},
                     {base + (coarse ? 5 : 10), false, recording::encodeSize(2, r.header.sizeScale), 0}};
        writer.append(r);
    }
}
void writeHourFixture(const std::filesystem::path &root) {
    recording::Hmc2Store writer(root);
    recording::Hmc2Record r;
    r.header = {"BTC-USD", "deep", hour, 100, 500, {}, 79};
    r.observedMs = hour;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'050;
    r.bidRowLo = r.askRowLo = 20'000;
    r.bidRowHi = r.askRowHi = 20'003;
    for (int c = 0; c < 24; ++c) {
        r.bucketStartMs = recording::kHmc2MinMs + int64_t(c) * hour;
        const uint32_t partial = c % 3 == 0 ? uint32_t(hour / 2) : uint32_t(hour);
        r.coverage = {{20'000, 20'001, false, uint32_t(hour)},
                      {20'002, 20'002, false, partial},
                      {20'003, 20'003, false, uint32_t(hour)},
                      {20'000, 20'003, true, uint32_t(hour)}};
        r.entries = {{20'001, false, recording::encodeSize(5 + c % 3), 0, uint32_t(hour)},
                     {20'002, false, recording::encodeSize(12 + c % 5), 0, partial},
                     {20'002, true, recording::encodeSize(8 + c % 2), 0, uint32_t(hour)}};
        writer.append(r);
    }
}
void writeMinuteTailFixture(const std::filesystem::path &root) {
    recording::Hmc2Store writer(root);
    recording::Hmc2Record r;
    r.header = {"BTC-USD", "deep", hour, 100, 500, {}, 91};
    r.bucketStartMs = recording::kHmc2MinMs;
    r.observedMs = hour;
    r.midOpen = r.midClose = r.midMin = r.midMax = 100'050;
    r.bidRowLo = r.askRowLo = 20'000;
    r.bidRowHi = r.askRowHi = 20'002;
    r.coverage = {{20'000, 20'002, false, uint32_t(hour)},
                  {20'000, 20'002, true, uint32_t(hour)}};
    r.entries = {{20'001, false, recording::encodeSize(10), 0, uint32_t(hour)}};
    writer.append(r);
    r.header.tfMs = minute;
    r.header.configHash = 92;
    r.coverage.clear();
    for (int c = 0; c < 30; ++c) {
        r.bucketStartMs = recording::kHmc2MinMs + hour + int64_t(c) * minute;
        r.observedMs = minute;
        r.bidRowHi = c % 2 ? 20'001 : 20'002;
        r.entries = {{20'001, false, recording::encodeSize(20 + c % 3), 0}};
        writer.append(r);
    }
}

struct Deviation { int code = 0; int side = 0; int validity = 0; int cells = 0; };

void compareViews(QRhi *rhi, const std::filesystem::path &root,
                  const recording::RecordingEntries &data, int64_t end, Deviation &deviation) {
    ASSERT_GT(data.rowSide.size(), 0u);
    ASSERT_GT(data.baseRow, 1000); // Exercises absolute price-LOD coordinates.
    lab::GpuBinner binner(rhi);
    QRhiCommandBuffer *cb = nullptr;
    QString error;
    if (data.rowSide.size() < 100'000) {
        ASSERT_TRUE(binner.beginUpload(data, &error)) << error.toStdString();
        int uploadFrames = 0;
        while (!binner.uploadComplete()) {
            ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
            ASSERT_TRUE(binner.uploadStep(cb, 64 * 1024, &error)) << error.toStdString();
            ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
            ASSERT_LT(++uploadFrames, 1000);
        }
        if (data.sourceMinutes == 1) EXPECT_GT(uploadFrames, 1);
    } else {
        ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        ASSERT_TRUE(binner.upload(cb, data, &error)) << error.toStdString();
        ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    }

    const std::vector<std::pair<int64_t, int64_t>> views = data.sourceMinutes == 60 ?
        std::vector<std::pair<int64_t, int64_t>>{{hour, hour}, {24 * hour, hour}, {8 * hour, 4 * hour},
                                                 {24 * hour, 24 * hour}} :
        data.sourceMinutes == 5 ?
        std::vector<std::pair<int64_t, int64_t>>{{hour, 5 * minute}} :
        data.sourceMinutes == 16 ?
        std::vector<std::pair<int64_t, int64_t>>{{16 * 16 * minute, 16 * minute}} :
        data.sourceMinutes == 240 ?
        std::vector<std::pair<int64_t, int64_t>>{{8 * hour, 4 * hour}} :
        data.sourceMinutes == 1440 ?
        std::vector<std::pair<int64_t, int64_t>>{{24 * hour, 24 * hour}} :
        std::vector<std::pair<int64_t, int64_t>>{{24 * hour, 15 * minute},
                                                 {hour, 5 * minute},
                                                 {16 * 16 * minute, 16 * minute},
                                                 {10 * minute, minute}};
    for (const auto [window, tf] : views) {
        const int64_t viewEnd = end / tf * tf;
        const int64_t viewStart = viewEnd - window;
        if (viewStart < data.startMs) continue;
        for (const int factor : {2, 10, 20, 40, 50, 200}) {
            const double tick = data.nativeTick * factor;
            const uint32_t rows = 12;
            int64_t middle = data.baseRow + 6;
            const int64_t first = int64_t(std::floor(double(middle) / factor)) * factor;
            recording::BuildRequest request;
            request.symbol = "BTC-USD";
            request.tfMs = tf;
            request.endMs = viewEnd - tf;
            request.count = uint32_t(window / tf);
            request.priceLo = first * data.nativeTick;
            request.priceHi = request.priceLo + rows * tick;
            request.rows = rows;
            request.displayTick = tick;
            request.budgets = {100'000, 100'000'000, 30'000};
            const auto page = recording::buildPage(root, request);
            ASSERT_EQ(page.status, recording::BuildStatus::Complete) << page.message;
            ASSERT_EQ(page.layer, "deep");
            ASSERT_EQ(page.band.rows, rows);

            lab::Grid grid;
            grid.firstMinute = int32_t((viewStart - data.startMs) / minute);
            grid.timeframeMinutes = uint32_t(tf / minute);
            grid.columns = uint32_t(window / tf);
            grid.rows = rows;
            grid.rowLo = int32_t(first - data.baseRow);
            grid.baseRow = int32_t(data.baseRow);
            grid.group = factor;
            grid.sizeFloor = float(data.sizeScale.floor);
            grid.codesPerOctave = float(data.sizeScale.codesPerOctave);
            QRhiReadbackResult readback;
            ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
            ASSERT_TRUE(binner.bin(cb, grid, &error)) << error.toStdString();
            ASSERT_TRUE(binner.readBack(cb, grid, &readback, &error)) << error.toStdString();
            ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
            ASSERT_EQ(readback.data.size(), qsizetype(grid.columns * grid.rows * 16));
            for (const auto &column : page.columns) {
                const int x = int((column.bucketStartMs - viewStart) / tf);
                if (x < 0 || x >= int(grid.columns)) continue;
                for (uint32_t y = 0; y < rows; ++y) {
                    float gpu[4];
                    std::memcpy(gpu, readback.data.constData() + (y * grid.columns + x) * 16, sizeof(gpu));
                    const auto cell = column.cells[y];
                    const int codeDelta = std::abs(int(recording::encodeSize(
                        std::max(double(gpu[0]), double(gpu[1])), page.sizeScale)) - int(cell & 0x7fffu));
                    const bool gpuSide = gpu[1] > gpu[0];
                    const bool pageSide = (cell & 0x8000u) != 0u;
                    const bool pageValid = (column.validity[y / 8] & (1u << (y % 8))) != 0;
                    if (codeDelta > 1 && deviation.code <= 1)
                        std::cerr << "parity deviation window_ms=" << window << " factor=" << factor
                                  << " bucket=" << column.bucketStartMs << " row=" << y
                                  << " gpu_bid=" << gpu[0] << " gpu_ask=" << gpu[1]
                                  << " gpu_code=" << recording::encodeSize(std::max(double(gpu[0]), double(gpu[1])), page.sizeScale)
                                  << " page_code=" << (cell & 0x7fffu) << " code_delta=" << codeDelta
                                  << " gpu_valid=" << gpu[2] << " page_valid=" << pageValid
                                  << " gpu_floor=" << data.sizeScale.floor << " page_floor=" << page.sizeScale.floor
                                  << " gpu_oct=" << data.sizeScale.codesPerOctave
                                  << " page_oct=" << page.sizeScale.codesPerOctave << '\n';
                    deviation.code = std::max(deviation.code, codeDelta);
                    deviation.side += gpuSide != pageSide;
                    deviation.validity += (gpu[2] > 0.5f) != pageValid;
                    ++deviation.cells;
                }
            }
        }
    }
}

TEST(RecordingGpuParity, SyntheticAndOptionalRecordedDeepMatchServer) {
    std::unique_ptr<QRhi> rhi;
#ifdef Q_OS_MACOS
    void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
    auto createDevice = metal ? reinterpret_cast<void *(*)()>(dlsym(metal, "MTLCreateSystemDefaultDevice")) : nullptr;
    const bool available = createDevice && createDevice();
    if (metal) dlclose(metal);
    if (!available) GTEST_SKIP() << "No MTLDevice; GPU readback parity requires unsandboxed Metal";
    QRhiMetalInitParams init;
    rhi.reset(QRhi::create(QRhi::Metal, &init));
#else
    GTEST_SKIP() << "GPU readback parity requires macOS Metal";
#endif
    ASSERT_TRUE(rhi);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root(dir.path().toStdString());
    writeFixture(root);
    const int64_t syntheticEnd = recording::kHmc2MinMs + 24 * hour;
    const auto older = recording::loadRecordingEntries(root, "BTC-USD", "deep",
                                                        recording::kHmc2MinMs, syntheticEnd - 12 * hour);
    const auto recent = recording::loadRecordingEntries(root, "BTC-USD", "deep",
                                                         syntheticEnd - 12 * hour, syntheticEnd);
    const auto fixture = recording::joinRecordingEntries(older, recent);
    Deviation synthetic;
    compareViews(rhi.get(), root, fixture, syntheticEnd, synthetic);
    std::cout << "synthetic GPU parity: cells=" << synthetic.cells << " max_code_delta=" << synthetic.code
              << " side_mismatches=" << synthetic.side << " validity_mismatches=" << synthetic.validity << '\n';
    EXPECT_GT(synthetic.cells, 0);
    EXPECT_LE(synthetic.code, 1);
    EXPECT_EQ(synthetic.side, 0);
    EXPECT_EQ(synthetic.validity, 0);

    for (uint32_t tf : {5u, 16u}) {
        const auto composed = recording::loadComposedMinuteEntries(root, "BTC-USD", "deep",
            recording::kHmc2MinMs, syntheticEnd, tf);
        ASSERT_EQ(composed.sourceMinutes, tf);
        Deviation selected;
        compareViews(rhi.get(), root, composed, syntheticEnd, selected);
        std::cout << tf << "m composed GPU parity: cells=" << selected.cells
                  << " max_code_delta=" << selected.code
                  << " side_mismatches=" << selected.side
                  << " validity_mismatches=" << selected.validity << '\n';
        EXPECT_GT(selected.cells, 0);
        EXPECT_LE(selected.code, 1);
        EXPECT_EQ(selected.side, 0);
        EXPECT_EQ(selected.validity, 0);
    }

    writeHourFixture(root);
    const auto hourData = recording::loadRecordingEntries(root, "BTC-USD", "deep",
        recording::kHmc2MinMs, syntheticEnd, 60);
    ASSERT_EQ(hourData.sourceMinutes, 60u);
    ASSERT_EQ(hourData.columns(), 24u);
    const auto hourOlder = recording::loadRecordingEntries(root, "BTC-USD", "deep",
        recording::kHmc2MinMs, recording::kHmc2MinMs + 12 * hour, 60);
    const auto hourRecent = recording::loadRecordingEntries(root, "BTC-USD", "deep",
        recording::kHmc2MinMs + 12 * hour, syntheticEnd, 60);
    const auto joinedHours = recording::joinRecordingEntries(hourOlder, hourRecent);
    EXPECT_EQ(joinedHours.rowSide, hourData.rowSide);
    EXPECT_EQ(joinedHours.entryCoveredMs, hourData.entryCoveredMs);
    EXPECT_EQ(joinedHours.coverageRuns, hourData.coverageRuns);
    EXPECT_EQ(joinedHours.coverageRunOffsets, hourData.coverageRunOffsets);
    Deviation hourly;
    compareViews(rhi.get(), root, joinedHours, syntheticEnd, hourly);
    std::cout << "hour-rollup GPU parity: cells=" << hourly.cells << " max_code_delta=" << hourly.code
              << " side_mismatches=" << hourly.side << " validity_mismatches=" << hourly.validity << '\n';
    EXPECT_GT(hourly.cells, 0);
    EXPECT_LE(hourly.code, 1);
    EXPECT_EQ(hourly.side, 0);
    EXPECT_EQ(hourly.validity, 0);

    for (uint32_t tf : {240u, 1440u}) {
        const auto composed = recording::loadComposedHourEntries(
            root, "BTC-USD", "deep", recording::kHmc2MinMs, syntheticEnd, tf);
        ASSERT_EQ(composed.sourceMinutes, tf);
        Deviation selected;
        compareViews(rhi.get(), root, composed, syntheticEnd, selected);
        std::cout << "synthetic " << tf << "m hour-composed GPU parity: cells="
                  << selected.cells << " max_code_delta=" << selected.code
                  << " side_mismatches=" << selected.side
                  << " validity_mismatches=" << selected.validity << '\n';
        EXPECT_GT(selected.cells, 0);
        EXPECT_LE(selected.code, 1);
        EXPECT_EQ(selected.side, 0);
        EXPECT_EQ(selected.validity, 0);
    }

    QTemporaryDir tailDir;
    ASSERT_TRUE(tailDir.isValid());
    const std::filesystem::path tailRoot(tailDir.path().toStdString());
    writeMinuteTailFixture(tailRoot);
    const auto withTail = recording::loadHourEntriesWithMinuteTail(
        tailRoot, "BTC-USD", "deep", recording::kHmc2MinMs, recording::kHmc2MinMs + 2 * hour);
    ASSERT_EQ(withTail.sourceMinutes, 60u);
    ASSERT_EQ(withTail.columns(), 2u);
    EXPECT_EQ(withTail.observedMs[1], 30 * minute);
    Deviation tail;
    compareViews(rhi.get(), tailRoot, withTail, recording::kHmc2MinMs + 2 * hour, tail);
    std::cout << "minute-tail GPU parity: cells=" << tail.cells << " max_code_delta=" << tail.code
              << " side_mismatches=" << tail.side << " validity_mismatches=" << tail.validity << '\n';
    EXPECT_GT(tail.cells, 0);
    EXPECT_LE(tail.code, 1);
    EXPECT_EQ(tail.side, 0);
    EXPECT_EQ(tail.validity, 0);

    // Relative rows above 65535 must use the six-byte packed fallback.
    recording::RecordingEntries wide;
    wide.startMs = recording::kHmc2MinMs;
    wide.baseRow = 20'000;
    wide.nativeTick = 1;
    wide.coverage = {{0, 70'000, 0, 70'000}};
    wide.observedMs = {uint32_t(minute)};
    wide.nativeFactor = {1};
    wide.columnScale = {wide.sizeScale};
    wide.rowSide = {70'000u};
    wide.code = {recording::encodeSize(7)};
    wide.offsets = {0, 1};
    lab::GpuBinner wideBinner(rhi.get());
    lab::Grid wideGrid;
    wideGrid.firstMinute = 0; wideGrid.timeframeMinutes = 1;
    wideGrid.rowLo = 70'000; wideGrid.baseRow = 20'000;
    wideGrid.group = wideGrid.columns = wideGrid.rows = 1;
    wideGrid.sizeFloor = float(wide.sizeScale.floor);
    wideGrid.codesPerOctave = float(wide.sizeScale.codesPerOctave);
    QRhiCommandBuffer *wideCb = nullptr;
    QRhiReadbackResult wideReadback;
    QString wideError;
    ASSERT_EQ(rhi->beginOffscreenFrame(&wideCb), QRhi::FrameOpSuccess);
    ASSERT_TRUE(wideBinner.upload(wideCb, wide, &wideError)) << wideError.toStdString();
    ASSERT_TRUE(wideBinner.bin(wideCb, wideGrid, &wideError)) << wideError.toStdString();
    ASSERT_TRUE(wideBinner.readBack(wideCb, wideGrid, &wideReadback, &wideError))
        << wideError.toStdString();
    ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    ASSERT_EQ(wideReadback.data.size(), 16);
    float wideCell[4];
    std::memcpy(wideCell, wideReadback.data.constData(), sizeof(wideCell));
    EXPECT_EQ(recording::encodeSize(wideCell[0]), recording::encodeSize(7));
    EXPECT_EQ(wideCell[2], 1.0f);

    if (qgetenv("SENTINEL_LAB_REAL_PARITY") != "1") {
        std::cout << "real GPU parity: skipped (set SENTINEL_LAB_REAL_PARITY=1 to opt in)\n";
        return;
    }
    const std::filesystem::path realRoot("/Volumes/T7/sentinel-data/recording");
    if (!std::filesystem::exists(realRoot / "BTC-USD")) {
        std::cout << "real GPU parity: skipped (recording directory absent)\n";
        return;
    }
    // The previous completed UTC day is immutable to the live recorder. Every
    // selected timeframe below divides a day, so no view includes a live tail.
    const int64_t realEnd = QDateTime::currentMSecsSinceEpoch() / (24 * hour) * (24 * hour);
    const auto real = recording::loadRecordingEntries(realRoot, "BTC-USD", "deep",
                                                       realEnd - 24 * hour, realEnd);
    if (real.rowSide.empty()) {
        std::cout << "real GPU parity: skipped (no deep entries in last 24 hours)\n";
        return;
    }
    Deviation recorded;
    compareViews(rhi.get(), realRoot, real, realEnd, recorded);
    std::cout << "real GPU parity: cells=" << recorded.cells << " max_code_delta=" << recorded.code
              << " side_mismatches=" << recorded.side << " validity_mismatches=" << recorded.validity << '\n';
    EXPECT_GT(recorded.cells, 0);
    EXPECT_LE(recorded.code, 1);
    EXPECT_EQ(recorded.side, 0);
    EXPECT_EQ(recorded.validity, 0);

    for (uint32_t tf : {5u, 16u}) {
        const int64_t selectedEnd = realEnd;
        const auto selectedData = recording::loadComposedMinuteEntries(
            realRoot, "BTC-USD", "deep", selectedEnd - 24 * hour, selectedEnd, tf);
        ASSERT_EQ(selectedData.sourceMinutes, tf);
        Deviation selected;
        compareViews(rhi.get(), realRoot, selectedData, selectedEnd, selected);
        std::cout << "real " << tf << "m GPU parity: cells=" << selected.cells
                  << " max_code_delta=" << selected.code
                  << " side_mismatches=" << selected.side
                  << " validity_mismatches=" << selected.validity << '\n';
        EXPECT_GT(selected.cells, 0);
        EXPECT_LE(selected.code, 1);
        EXPECT_EQ(selected.side, 0);
        EXPECT_EQ(selected.validity, 0);
    }

    const int64_t hourEnd = realEnd;
    const auto realHours = recording::loadRecordingEntries(realRoot, "BTC-USD", "deep",
                                                            hourEnd - 24 * hour, hourEnd, 60);
    auto lastHour = std::find_if(realHours.observedMs.rbegin(), realHours.observedMs.rend(),
                                 [](uint32_t ms) { return ms != 0; });
    if (lastHour != realHours.observedMs.rend()) {
        const int64_t lastExclusive = realHours.startMs +
            int64_t(realHours.observedMs.rend() - lastHour) * hour;
        Deviation rollup;
        compareViews(rhi.get(), realRoot, realHours, lastExclusive, rollup);
        std::cout << "real hour-rollup GPU parity: cells=" << rollup.cells
                  << " max_code_delta=" << rollup.code
                  << " side_mismatches=" << rollup.side
                  << " validity_mismatches=" << rollup.validity << '\n';
        EXPECT_GT(rollup.cells, 0);
        EXPECT_LE(rollup.code, 1);
        EXPECT_EQ(rollup.side, 0);
        EXPECT_EQ(rollup.validity, 0);
    }
    for (uint32_t tf : {240u, 1440u}) {
        const int64_t tfMs = int64_t(tf) * minute;
        const int64_t selectedEnd = realEnd;
        const auto selectedData = recording::loadComposedHourEntries(
            realRoot, "BTC-USD", "deep", selectedEnd - 2 * tfMs, selectedEnd, tf);
        if (selectedData.rowSide.empty()) continue;
        ASSERT_EQ(selectedData.sourceMinutes, tf);
        Deviation selected;
        compareViews(rhi.get(), realRoot, selectedData, selectedEnd, selected);
        std::cout << "real " << tf << "m hour-composed GPU parity: cells="
                  << selected.cells << " max_code_delta=" << selected.code
                  << " side_mismatches=" << selected.side
                  << " validity_mismatches=" << selected.validity << '\n';
        EXPECT_GT(selected.cells, 0);
        EXPECT_LE(selected.code, 1);
        EXPECT_EQ(selected.side, 0);
        EXPECT_EQ(selected.validity, 0);
    }
}
} // namespace
