#include "lab/GpuBinner.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QDateTime>
#include <QTemporaryDir>
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
        EXPECT_GT(uploadFrames, 1);
    } else {
        ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        ASSERT_TRUE(binner.upload(cb, data, &error)) << error.toStdString();
        ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    }

    for (const auto [window, tf] : {std::pair{24 * hour, 15 * minute},
                                    {hour, 5 * minute}, {10 * minute, minute}}) {
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
            grid.timeLo = float((viewStart - data.startMs) / minute);
            grid.timeHi = float((viewEnd - data.startMs) / minute);
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

TEST(RecordingGpuParity, SyntheticAndRecordedDeepMatchServer) {
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
    const auto fixture = recording::loadRecordingEntries(root, "BTC-USD", "deep",
                                                          recording::kHmc2MinMs, syntheticEnd);
    Deviation synthetic;
    compareViews(rhi.get(), root, fixture, syntheticEnd, synthetic);
    std::cout << "synthetic GPU parity: cells=" << synthetic.cells << " max_code_delta=" << synthetic.code
              << " side_mismatches=" << synthetic.side << " validity_mismatches=" << synthetic.validity << '\n';
    EXPECT_GT(synthetic.cells, 0);
    EXPECT_LE(synthetic.code, 1);
    EXPECT_EQ(synthetic.side, 0);
    EXPECT_EQ(synthetic.validity, 0);

    const std::filesystem::path realRoot("/Volumes/T7/sentinel-data/recording");
    if (!std::filesystem::exists(realRoot / "BTC-USD")) {
        std::cout << "real GPU parity: skipped (recording directory absent)\n";
        return;
    }
    const int64_t realEnd = QDateTime::currentMSecsSinceEpoch() / (15 * minute) * (15 * minute);
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
}
} // namespace
