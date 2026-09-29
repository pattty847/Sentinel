// S4: GPU price binning parity against heatmap::binColumn (the CPU reference),
// plus grid/threshold/source unit tests and a scene-graph render-node test.
// GPU cases skip cleanly without a Metal device. Real-recording parity is opt-in:
//   SENTINEL_HEATMAP_REAL_PARITY=1 ./test_heatmap_gpu_binner
#include "heatmap/BinCell.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "heatmap/RecordingLoader.hpp"
#include "heatmap/TimeComposer.hpp"
#include "lab/OffscreenQuick.hpp"
#include "render/heatmap/HeatmapGpuBinner.hpp"
#include "render/heatmap/HeatmapGpuSelfTest.hpp"
#include "render/heatmap/HeatmapRenderNode.hpp"
#include <QDateTime>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <thread>

namespace {
using namespace heatmap;
using namespace heatmap::gpu;
constexpr int64_t minute = kMinuteMs, hour = kHourMs, day = kDayMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 30 * day; // any UTC-day-aligned origin

// ---------------------------------------------------------------- fixtures
// Minute records with a grid change ($10 -> $5 at minute 360), a config and
// size-scale change on the same $5 tick at 540, partial minutes, partial side
// coverage and deterministic sizes. Gap minutes are simply not produced.
recording::Hmc2Record makeMinute(int64_t index) {
    recording::Hmc2Record r;
    const int64_t tick = index < 360 ? 10 : 5;
    r.header = {"BTC-USD", "deep", minute, 100, tick * 100, {}, uint64_t(index < 360 ? 41 : 42)};
    if (index >= 540) { r.header.sizeScale.floor = 1e-8; r.header.configHash = 43; }
    r.bucketStartMs = epoch + index * minute;
    r.observedMs = index % 11 ? uint32_t(minute) : 17'123;
    r.flags = r.observedMs < minute ? recording::kPartial : 0;
    r.bidRowLo = 100'000 / tick;
    r.askRowLo = (100'000 + (index % 7 == 0 ? 10 : 0)) / tick;
    r.bidRowHi = (100'040 - (index % 5 == 0 ? 10 : 0)) / tick - 1;
    r.askRowHi = 100'040 / tick - 1;
    for (int64_t row = 100'000 / tick; row < 100'030 / tick; ++row)
        for (bool ask : {false, true}) {
            if (row < (ask ? r.askRowLo : r.bidRowLo) || row > (ask ? r.askRowHi : r.bidRowHi)) continue;
            if ((row * 3 + index) % 9 == 4) continue; // covered zero rows
            const double size = 0.000013 * (1 + (row * 17 + index * 13 + int(ask) * 19) % 127) *
                                (1 + (index % 17) * 0.37);
            const auto code = recording::encodeSize(size, r.header.sizeScale);
            r.entries.push_back({row, ask, code, code, r.observedMs});
        }
    return r;
}
bool isGapMinute(int64_t index) { return index % 13 == 5; }

// Independent dense hour rollup (quantized like persisted schema-4 hours).
recording::Hmc2Record makeHour(const std::vector<recording::Hmc2Record> &minutes) {
    auto out = minutes.front();
    out.header.tfMs = hour;
    out.bucketStartMs = out.bucketStartMs / hour * hour;
    out.entries.clear(); out.coverage.clear(); out.observedMs = 0; out.flags = 0;
    out.bidRowLo = out.askRowLo = INT64_MAX;
    out.bidRowHi = out.askRowHi = 0;
    for (const auto &r : minutes) {
        out.observedMs += r.observedMs;
        out.bidRowLo = std::min(out.bidRowLo, r.bidRowLo); out.bidRowHi = std::max(out.bidRowHi, r.bidRowHi);
        out.askRowLo = std::min(out.askRowLo, r.askRowLo); out.askRowHi = std::max(out.askRowHi, r.askRowHi);
    }
    for (bool ask : {false, true})
        for (int64_t row = ask ? out.askRowLo : out.bidRowLo; row <= (ask ? out.askRowHi : out.bidRowHi); ++row) {
            uint32_t covered = 0;
            long double numerator = 0;
            for (const auto &r : minutes) {
                if (row < (ask ? r.askRowLo : r.bidRowLo) || row > (ask ? r.askRowHi : r.bidRowHi)) continue;
                covered += r.observedMs;
                for (const auto &e : r.entries)
                    if (e.row == row && e.isAsk == ask)
                        numerator += (long double)recording::decodeSize(e.twapCode, r.header.sizeScale) * r.observedMs;
            }
            if (!covered) continue;
            if (!out.coverage.empty() && out.coverage.back().isAsk == ask &&
                out.coverage.back().hi + 1 == row && out.coverage.back().coveredMs == covered)
                out.coverage.back().hi = row;
            else out.coverage.push_back({row, row, ask, covered});
            if (numerator) {
                const auto code = recording::encodeSize(double(numerator / covered), out.header.sizeScale);
                out.entries.push_back({row, ask, code, code, covered});
            }
        }
    std::sort(out.entries.begin(), out.entries.end(), [](const auto &a, const auto &b) {
        return std::pair(a.row, a.isAsk) < std::pair(b.row, b.isAsk);
    });
    return out;
}

// One UTC day of minutes. Minutes [600, 660) were never loaded (NotLoaded).
SparseColumns minuteLevel() {
    SparseColumns out{"BTC-USD", "deep", minute, epoch, epoch + day, {}, {}};
    out.scannedRanges = {{epoch, epoch + 600 * minute}, {epoch + 660 * minute, epoch + day}};
    for (int64_t i = 0; i < 1440; ++i) {
        if (isGapMinute(i) || (i >= 600 && i < 660)) continue;
        out.columns.push_back(fromRecording(makeMinute(i)));
    }
    validate(out);
    return out;
}
// Persisted hours for the first 8 hours (they supersede minutes there).
SparseColumns hourLevel() {
    SparseColumns out{"BTC-USD", "deep", hour, epoch, epoch + 8 * hour, {}, {{epoch, epoch + 8 * hour}}};
    for (int64_t h = 0; h < 8; ++h) {
        std::vector<recording::Hmc2Record> minutes;
        for (int64_t i = h * 60; i < h * 60 + 60; ++i)
            if (!isGapMinute(i)) minutes.push_back(makeMinute(i));
        out.columns.push_back(fromRecording(makeHour(minutes)));
    }
    validate(out);
    return out;
}

// ---------------------------------------------------------------- GPU helpers
struct Headless {
    std::unique_ptr<QRhi> rhi;
    Headless() {
        if (!lab::metalDeviceAvailable()) return;
#ifdef Q_OS_MACOS
        QRhiMetalInitParams init;
        rhi.reset(QRhi::create(QRhi::Metal, &init));
#endif
    }
};
void uploadPaged(QRhi *rhi, HeatmapGpuBinner &binner, std::shared_ptr<const GpuSource> source,
                 uint64_t budget, int *frames = nullptr) {
    QString error;
    ASSERT_TRUE(binner.setSource(std::move(source), &error)) << error.toStdString();
    int count = 0;
    while (binner.uploadPending()) {
        QRhiCommandBuffer *cb = nullptr;
        ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        ASSERT_TRUE(binner.uploadStep(cb, budget, &error)) << error.toStdString();
        ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        ASSERT_LT(++count, 100000);
    }
    if (frames) *frames = count;
}
std::vector<uint32_t> binAndRead(QRhi *rhi, HeatmapGpuBinner &binner, const BinGrid &grid,
                                 const recording::SizeScale &scale = {}) {
    QString error;
    QRhiCommandBuffer *cb = nullptr;
    QRhiReadbackResult readback;
    EXPECT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    EXPECT_TRUE(binner.bin(cb, grid, scale, &error)) << error.toStdString();
    EXPECT_TRUE(binner.readBack(cb, &readback, &error)) << error.toStdString();
    EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    std::vector<uint32_t> cells(size_t(grid.columns) * grid.rows);
    if (readback.data.size() == qsizetype(cells.size() * 4)) std::memcpy(cells.data(), readback.data.constData(), cells.size() * 4);
    else ADD_FAILURE() << "readback size " << readback.data.size();
    return cells;
}

struct Tally {
    uint64_t cells = 0, valid = 0, veil = 0, loading = 0, noData = 0;
    uint64_t stateMismatch = 0, codeMismatch = 0, sideMismatch = 0, validityMismatch = 0;
    void add(const Tally &o) {
        cells += o.cells; valid += o.valid; veil += o.veil; loading += o.loading; noData += o.noData;
        stateMismatch += o.stateMismatch; codeMismatch += o.codeMismatch;
        sideMismatch += o.sideMismatch; validityMismatch += o.validityMismatch;
    }
    bool exact() const { return !stateMismatch && !codeMismatch && !sideMismatch && !validityMismatch; }
};
std::ostream &operator<<(std::ostream &os, const Tally &t) {
    return os << "cells=" << t.cells << " valid=" << t.valid << " veil=" << t.veil << " loading=" << t.loading
              << " no_data=" << t.noData << " | mismatches state=" << t.stateMismatch << " code=" << t.codeMismatch
              << " side=" << t.sideMismatch << " validity=" << t.validityMismatch;
}

// Compare every GPU cell with the CPU reference (bucketState + binColumn).
Tally compareGrid(const SparseColumns &data, const GpuSource &source, const BinGrid &grid,
                  const std::vector<uint32_t> &cells, const std::string &label,
                  const recording::SizeScale &scale = {}) {
    Tally t;
    int reported = 0;
    const double tick = grid.displayTick;
    const double clipLo = std::ceil(source.clipPriceLo / tick - 1e-9), clipEnd = std::floor(source.clipPriceHi / tick + 1e-9);
    for (uint32_t x = 0; x < grid.columns; ++x) {
        const int64_t bucket = grid.firstBucket + x;
        const int64_t startMs = bucket * data.tfMs;
        std::vector<BinCell> expected;
        CellState columnState;
        if (bucket < source.availableFirstBucket || bucket >= source.availableEndBucket) columnState = CellState::NoData;
        else {
            const auto state = bucketState(data, startMs);
            columnState = state == BucketState::NotLoaded ? CellState::Loading :
                          state == BucketState::Gap ? CellState::Veil : CellState::Valid;
            if (state == BucketState::Present) {
                const auto it = std::lower_bound(data.columns.begin(), data.columns.end(), startMs,
                    [](const SparseColumn &c, int64_t s) { return c.bucketStartMs < s; });
                expected = binColumn(*it, double(grid.firstBin) * tick, double(grid.firstBin + grid.rows) * tick, tick, scale);
                if (expected.size() != grid.rows) { ADD_FAILURE() << "binColumn rows"; return t; }
            }
        }
        for (uint32_t y = 0; y < grid.rows; ++y) {
            const Cell got = decodeCell(cells[size_t(y) * grid.columns + x]);
            const int64_t bin = grid.firstBin + int64_t(grid.rows - 1 - y);
            CellState want = columnState;
            if (want != CellState::NoData && (bin < clipLo || bin >= clipEnd) && want != CellState::NoData &&
                columnState != CellState::Loading)
                want = CellState::Loading;
            if (want == CellState::Valid && !expected.empty() && !expected[y].valid) want = CellState::Veil;
            ++t.cells;
            switch (got.state) {
            case CellState::Valid: ++t.valid; break;
            case CellState::Veil: ++t.veil; break;
            case CellState::Loading: ++t.loading; break;
            case CellState::NoData: ++t.noData; break;
            }
            const bool present = !expected.empty() && want != CellState::Loading;
            if (got.state != want) {
                const bool validity = present && (got.state == CellState::Valid || got.state == CellState::Veil) &&
                                      (want == CellState::Valid || want == CellState::Veil);
                ++(validity ? t.validityMismatch : t.stateMismatch);
            }
            uint16_t wantCode = 0;
            bool wantAsk = false;
            if (present) {
                wantCode = expected[y].code & recording::kMaxCode;
                wantAsk = expected[y].dominantAsk;
            }
            const bool codeBad = got.code != wantCode, sideBad = got.ask != wantAsk;
            t.codeMismatch += codeBad;
            t.sideMismatch += sideBad;
            if ((codeBad || sideBad || got.state != want) && reported++ < 5) {
                std::cerr << label << " mismatch bucket=" << startMs << " bin=" << bin << " tick=" << tick
                          << " gpu{state=" << int(got.state) << " code=" << got.code << " ask=" << got.ask << "}"
                          << " cpu{state=" << int(want) << " code=" << wantCode << " ask=" << wantAsk;
                if (present) std::cerr << std::setprecision(17) << " bid=" << expected[y].bid << " ask=" << expected[y].ask;
                std::cerr << "}\n";
            }
        }
    }
    return t;
}

// Grid over the whole source time extent plus `pad` buckets, and `rows` display
// bins starting at `firstPrice`.
BinGrid gridFor(const GpuSource &source, double tick, double firstPrice, uint32_t rows, int64_t pad) {
    BinGrid grid;
    grid.tfMs = source.tfMs;
    grid.firstBucket = source.firstBucket - pad;
    grid.columns = uint32_t(source.bucketSlots.size() + 2 * pad);
    grid.displayTick = tick;
    grid.firstBin = int64_t(std::floor(firstPrice / tick));
    grid.rows = rows;
    return grid;
}

// ---------------------------------------------------------------- CPU tests
TEST(HeatmapGpuSourceCpu, ThresholdTableReproducesEncodeSize) {
    for (const recording::SizeScale scale : {recording::SizeScale{}, recording::SizeScale{1e-8, 819}}) {
        const auto table = encodeThresholds(scale);
        ASSERT_EQ(table.size(), size_t(recording::kMaxCode - 1));
        auto gpuEncode = [&](double v) -> uint16_t {
            if (!(v > 0)) return 0;
            const auto ff = splitDouble(v);
            size_t a = 0, b = table.size();
            while (a < b) {
                const size_t m = (a + b) / 2;
                const bool ge = ff.hi > table[m].hi || (ff.hi == table[m].hi && ff.lo >= table[m].lo);
                if (ge) a = m + 1; else b = m;
            }
            return uint16_t(1 + a);
        };
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<double> exponent(-2, 45);
        int mismatches = 0;
        for (int i = 0; i < 200'000; ++i) {
            const double v = scale.floor * std::exp2(exponent(rng));
            mismatches += gpuEncode(v) != recording::encodeSize(v, scale);
        }
        EXPECT_EQ(mismatches, 0);
        // Just above and below each boundary. The float-float table resolves a
        // boundary to ~2^-48; GPU sums carry ~2^-38, so 1e-13 is the honest margin.
        for (uint32_t k = 2; k <= recording::kMaxCode; k += 97) {
            const double t = double(table[k - 2].hi) + double(table[k - 2].lo);
            const double above = t * (1 + 1e-13), below = t * (1 - 1e-13);
            EXPECT_EQ(recording::encodeSize(above, scale), k);
            EXPECT_EQ(gpuEncode(above), k);
            EXPECT_EQ(recording::encodeSize(below, scale), k - 1);
            EXPECT_EQ(gpuEncode(below), k - 1);
        }
    }
}

TEST(HeatmapGpuSourceCpu, CompactLowPartKeepsThirtyEightBits) {
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> exponent(-40, 30);
    for (int i = 0; i < 100'000; ++i) {
        const double v = std::exp2(exponent(rng)) * (1 + 1e-3 * i);
        const auto ff = splitDouble(v);
        const double back = double(ff.hi) + double(dequantizeLow(ff.hi, quantizeLow(ff.hi, ff.lo)));
        ASSERT_LE(std::abs(back - v), std::ldexp(v, -37)) << v;
    }
}

TEST(HeatmapGpuSourceCpu, SlotsDistinguishNotLoadedGapAndPresent) {
    const auto data = compose(minuteLevel(), 5 * minute);
    const auto source = buildGpuSource(data);
    ASSERT_EQ(source.tfMs, 5 * minute);
    ASSERT_EQ(source.bucketSlots.size(), 288u);
    for (size_t s = 0; s < source.bucketSlots.size(); ++s) {
        const auto state = bucketState(data, (source.firstBucket + int64_t(s)) * 5 * minute);
        const uint32_t slot = source.bucketSlots[s];
        if (state == BucketState::NotLoaded) EXPECT_EQ(slot, kSlotNotLoaded) << s;
        else if (state == BucketState::Gap) EXPECT_EQ(slot, kSlotGap) << s;
        else EXPECT_LT(slot, source.columns()) << s;
    }
    EXPECT_EQ(source.ticks.size(), 2u); // $5 and $10 native grids
    EXPECT_FALSE(source.wide);
    EXPECT_GT(source.entryCount, 0u);
}

TEST(HeatmapGpuSourceCpu, WideEntriesWhenRowSpanExceedsSixteenBits) {
    SparseColumns data{"BTC-USD", "near", minute, epoch, epoch + minute, {}, {{epoch, epoch + minute}}};
    NativeColumn n;
    n.grid = {1, 1, 100}; // $0.01 tick
    n.observedMs = minute;
    n.baseRow = 1'000'000;
    n.coverage[0] = {{1'000'000, 1'100'000, uint64_t(minute)}};
    n.entries = {{packRowSide(1'000'000, n.baseRow, false), recording::encodeSize(1)},
                 {packRowSide(1'090'000, n.baseRow, false), recording::encodeSize(2)}};
    data.columns.push_back({epoch, uint64_t(minute), 0, {n}});
    const auto source = buildGpuSource(compose(data, minute));
    EXPECT_TRUE(source.wide);
    EXPECT_EQ(source.entries.size(), 6u);
}

TEST(HeatmapBinGrid, SubBinPanStaysInsideGuardAndTranslatesMapping) {
    const ViewWindow view{double(epoch), double(epoch + 100 * minute), 100'000, 100'500};
    const auto grid = planGrid(view, minute, 10);
    ASSERT_TRUE(grid);
    EXPECT_EQ(grid->firstBucket, epoch / minute - 2);
    EXPECT_EQ(grid->columns, 104u);
    EXPECT_EQ(grid->firstBin, 10'000 - 2);
    EXPECT_TRUE(gridCovers(*grid, view, minute, 10));
    ViewWindow panned = view;
    panned.timeLoMs += 0.4 * minute; panned.timeHiMs += 0.4 * minute;
    panned.priceLo += 3.3; panned.priceHi += 3.3;
    EXPECT_TRUE(gridCovers(*grid, panned, minute, 10));
    const auto a = mappingFor(*grid, view), b = mappingFor(*grid, panned);
    EXPECT_NEAR(b.timeOffset - a.timeOffset, 0.4, 1e-4);
    EXPECT_NEAR(a.priceOffset - b.priceOffset, 0.33, 1e-4);
    EXPECT_FLOAT_EQ(a.timeSpan, b.timeSpan);
    ViewWindow far = view;
    far.timeLoMs += 3 * minute; far.timeHiMs += 3 * minute;
    EXPECT_FALSE(gridCovers(*grid, far, minute, 10));
    EXPECT_FALSE(gridCovers(*grid, view, minute, 20));
    EXPECT_FALSE(gridCovers(*grid, view, 5 * minute, 10));
}

// ---------------------------------------------------------------- GPU parity
TEST(HeatmapGpuParity, SyntheticMatchesBinColumnExactly) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice; GPU readback parity requires Metal";
    const auto minutes = minuteLevel();
    const auto hours = hourLevel();
    const std::vector<SparseColumns> levels{hours, minutes};
    struct Case { std::string label; SparseColumns data; };
    std::vector<Case> cases;
    for (const int64_t tf : {minute, 5 * minute, 16 * minute, hour})
        cases.push_back({"minutes tf=" + std::to_string(tf / minute) + "m", compose(minutes, tf)});
    for (const int64_t tf : {hour, 4 * hour})
        cases.push_back({"hours+minutes tf=" + std::to_string(tf / minute) + "m", compose(levels, tf)});
    Tally total;
    for (const auto &c : cases) {
        for (const bool clipped : {false, true}) {
            GpuSourceOptions options;
            options.availableStartMs = c.data.startMs - 3 * c.data.tfMs;  // 3 loading buckets before
            options.availableEndMs = c.data.endMs + 2 * c.data.tfMs;      // 2 loading buckets after
            if (clipped) { options.priceLo = 100'010; options.priceHi = 100'030; }
            auto source = std::make_shared<const GpuSource>(buildGpuSource(c.data, options));
            HeatmapGpuBinner binner(gpu.rhi.get());
            int frames = 0;
            uploadPaged(gpu.rhi.get(), binner, source, 4096, &frames);
            if (source->bytes() > 3 * 4096) EXPECT_GT(frames, 2) << "paged upload should need several frames";
            Tally tally;
            // $5, $10, $20, $50 and an incompatible $7.50; rows extend above and
            // below the covered band; time extends past both availability edges.
            for (const double tick : {5.0, 10.0, 20.0, 50.0, 7.5}) {
                const auto grid = gridFor(*source, tick, 99'900, uint32_t(std::ceil(250 / tick)) + 2, 5);
                const auto cells = binAndRead(gpu.rhi.get(), binner, grid);
                tally.add(compareGrid(c.data, *source, grid, cells, c.label));
            }
            std::cout << c.label << (clipped ? " clipped" : "") << ": " << tally << '\n';
            EXPECT_TRUE(tally.exact()) << c.label;
            EXPECT_GT(tally.valid, 0u);
            EXPECT_GT(tally.veil, 0u);
            EXPECT_GT(tally.loading, 0u);
            EXPECT_GT(tally.noData, 0u);
            total.add(tally);
        }
    }
    std::cout << "synthetic GPU parity total: " << total << '\n';
    EXPECT_TRUE(total.exact());
}

// Many entries per bin over a wide dynamic range: plain float sums flip codes
// here (verified: the float-only and the un-laundered fast-math shaders fail
// this test), so the always-on suite guards the float-float precision contract.
TEST(HeatmapGpuParity, PrecisionStressManyEntriesPerBin) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    SparseColumns data{"BTC-USD", "near", minute, epoch, epoch + 240 * minute, {}, {{epoch, epoch + 240 * minute}}};
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<int> code(1, 26'000);
    for (int64_t i = 0; i < 240; ++i) {
        NativeColumn n;
        n.grid = {7, 100, 100}; // $1
        n.observedMs = minute - (i % 3) * 7'001;
        n.baseRow = 100'000;
        n.coverage[0] = {{100'000, 101'999, n.observedMs}};
        n.coverage[1] = {{100'000, 101'999, n.observedMs}};
        for (int64_t row = 100'000; row < 102'000; ++row)
            for (bool ask : {false, true})
                if ((row + i + ask) % 5) n.entries.push_back({packRowSide(row, n.baseRow, ask), uint16_t(code(rng))});
        data.columns.push_back({epoch + i * minute, n.observedMs, 0, {n}});
    }
    validate(data);
    Tally total;
    for (const int64_t tf : {minute, 5 * minute}) {
        const auto composed = compose(data, tf);
        auto source = std::make_shared<const GpuSource>(buildGpuSource(composed));
        HeatmapGpuBinner binner(gpu.rhi.get());
        uploadPaged(gpu.rhi.get(), binner, source, 1 << 20);
        for (const double tick : {1.0, 5.0, 20.0, 100.0, 250.0}) {
            const auto grid = gridFor(*source, tick, 100'000, uint32_t(2000 / tick), 0);
            total.add(compareGrid(composed, *source, grid, binAndRead(gpu.rhi.get(), binner, grid), "stress"));
        }
    }
    std::cout << "precision stress: " << total << '\n';
    EXPECT_GT(total.valid, 100'000u);
    EXPECT_TRUE(total.exact());
}

TEST(HeatmapGpuParity, OutputResizeKeepsSourceAndNewSourceSwapsOnlyWhenComplete) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    const auto data = compose(minuteLevel(), minute);
    auto first = std::make_shared<const GpuSource>(buildGpuSource(data));
    HeatmapGpuBinner binner(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), binner, first, 1 << 20);
    const uint64_t sourceBytes = binner.sourceBytes();
    const auto small = gridFor(*first, 10, 99'990, 8, 0);
    auto big = gridFor(*first, 5, 99'950, 40, 50);
    const auto a = binAndRead(gpu.rhi.get(), binner, small);
    const auto b = binAndRead(gpu.rhi.get(), binner, big);      // output grows
    const auto c = binAndRead(gpu.rhi.get(), binner, small);    // and shrinks back
    EXPECT_EQ(a, c);
    EXPECT_EQ(binner.activeSource(), first);
    EXPECT_EQ(binner.sourceBytes(), sourceBytes);
    // A second source pages in over several frames; until complete the first stays active.
    auto second = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), 5 * minute)));
    QString error;
    ASSERT_TRUE(binner.setSource(second, &error));
    QRhiCommandBuffer *cb = nullptr;
    ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    ASSERT_TRUE(binner.uploadStep(cb, 1024, &error));
    ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    EXPECT_TRUE(binner.uploadPending());
    EXPECT_EQ(binner.activeSource(), first);
    EXPECT_EQ(binAndRead(gpu.rhi.get(), binner, small), a);
    uploadPaged(gpu.rhi.get(), binner, second, 1 << 20);
    EXPECT_EQ(binner.activeSource(), second);
}

TEST(HeatmapGpuParity, OutputScaleChangeRebinsWithMatchingCodes) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    const auto data = compose(minuteLevel(), 5 * minute);
    auto source = std::make_shared<const GpuSource>(buildGpuSource(data));
    HeatmapGpuBinner binner(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), binner, source, 1 << 20);
    const auto grid = gridFor(*source, 10, 99'900, 30, 2);
    const recording::SizeScale fine{1e-8, 819}, coarse{1e-5, 409};
    Tally tally;
    for (const auto &scale : {recording::SizeScale{}, fine, coarse, recording::SizeScale{}}) {
        const auto cells = binAndRead(gpu.rhi.get(), binner, grid, scale);
        EXPECT_TRUE(binner.binnedMatches(source->id, scale));
        tally.add(compareGrid(data, *source, grid, cells, "scale", scale));
    }
    EXPECT_FALSE(binner.binnedMatches(source->id, fine));
    std::cout << "output scale changes: " << tally << '\n';
    EXPECT_TRUE(tally.exact());
    EXPECT_GT(tally.valid, 0u);
}

TEST(HeatmapGpuParity, ReRequestingActiveSourceCancelsPendingUpload) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    auto a = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), minute)));
    auto b = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), 5 * minute)));
    HeatmapGpuBinner binner(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), binner, a, 1 << 20);
    QString error;
    auto step = [&](uint64_t budget) {
        QRhiCommandBuffer *cb = nullptr;
        ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        ASSERT_TRUE(binner.uploadStep(cb, budget, &error)) << error.toStdString();
        ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    };
    ASSERT_TRUE(binner.setSource(b, &error));      // A active, B pending
    step(512);
    step(512);
    ASSERT_TRUE(binner.uploadPending());
    EXPECT_EQ(binner.pendingSource(), b);
    ASSERT_TRUE(binner.setSource(a, &error));      // back to A: B must never activate
    EXPECT_FALSE(binner.uploadPending());
    EXPECT_EQ(binner.pendingSource(), nullptr);
    for (int i = 0; i < 4; ++i) step(1 << 20);
    EXPECT_EQ(binner.activeSource(), a);
    // B can still be requested afterwards and completes normally.
    uploadPaged(gpu.rhi.get(), binner, b, 1 << 20);
    EXPECT_EQ(binner.activeSource(), b);
    // A source the GPU cannot take is refused without disturbing the active one.
    auto tooManyPages = std::make_shared<GpuSource>(*a);
    tooManyPages->entryPageShift = 2; // thousands of pages
    EXPECT_FALSE(binner.setSource(tooManyPages, &error));
    EXPECT_FALSE(binner.uploadPending());
    EXPECT_EQ(binner.activeSource(), b);
    EXPECT_FALSE(binAndRead(gpu.rhi.get(), binner, gridFor(*b, 10, 99'990, 8, 0)).empty());
    // A newer request replaces a pending one (at most one pending source).
    auto c = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), 15 * minute)));
    ASSERT_TRUE(binner.setSource(a, &error));
    step(256);
    ASSERT_TRUE(binner.setSource(c, &error));
    EXPECT_EQ(binner.pendingSource(), c);
    uploadPaged(gpu.rhi.get(), binner, c, 1 << 20);
    EXPECT_EQ(binner.activeSource(), c);
}

TEST(HeatmapGpuParity, BufferPoolIsReusedAcrossSources) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    auto big = [] { return std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), minute))); };
    auto small = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), hour)));
    HeatmapGpuBinner binner(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), binner, big(), 1 << 20); // set 1 grows to "big"
    uploadPaged(gpu.rhi.get(), binner, big(), 1 << 20); // set 2 grows to "big"
    const uint64_t settled = binner.gpuBytes();         // two sets: active + spare
    for (int i = 0; i < 4; ++i) uploadPaged(gpu.rhi.get(), binner, i % 2 ? small : big(), 1 << 20);
    EXPECT_EQ(binner.gpuBytes(), settled) << "grow-only pool must not reallocate for sources that fit";
}

TEST(HeatmapGpuParity, EntriesSplitAcrossPagesMatchSinglePage) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    const auto data = compose(minuteLevel(), minute);
    const auto single = buildGpuSource(data);
    GpuSourceOptions options;
    uint32_t shift = 4;
    while ((single.entryCount >> shift) > 5) ++shift; // about six pages
    options.entryPageShift = shift;
    auto paged = std::make_shared<const GpuSource>(buildGpuSource(data, options));
    ASSERT_GE(paged->entryPages(), 3u);
    ASSERT_LE(paged->entryPages(), kMaxEntryPages);
    HeatmapGpuBinner a(gpu.rhi.get()), b(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), a, std::make_shared<const GpuSource>(single), 1 << 20);
    uploadPaged(gpu.rhi.get(), b, paged, 1 << 20);
    const auto grid = gridFor(single, 5, 99'900, 60, 2);
    const auto ca = binAndRead(gpu.rhi.get(), a, grid), cb = binAndRead(gpu.rhi.get(), b, grid);
    EXPECT_EQ(ca, cb);
    const auto tally = compareGrid(data, *paged, grid, cb, "paged");
    std::cout << "paged entries (" << paged->entryPages() << " pages): " << tally << '\n';
    EXPECT_TRUE(tally.exact());
}

// Precision self-test: the shipped fast kernel passes, the precise kernel is
// exact, and a kernel that lets fast math fold the error terms is rejected.
// The fixture builds on a worker, so resolution can take a few frames.
void settleSelfTest(QRhi *rhi, HeatmapGpuBinner &binner) {
    for (int i = 0; i < 500 && !binner.resolvedKernel(); ++i) {
        QRhiCommandBuffer *cb = nullptr;
        ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        binner.runPrecisionSelfTest(cb);
        ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        if (!binner.resolvedKernel()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
std::shared_ptr<const GpuSource> smallSource() {
    return std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), hour)));
}
TEST(HeatmapGpuSelfTest, FixtureOracleIsSelfConsistent) {
    const auto test = makePrecisionSelfTest();
    EXPECT_GE(test.expected.size(), 300u);
    EXPECT_EQ(countSelfTestMismatches(test, test.expected), 0u);
}
TEST(HeatmapGpuSelfTest, ShippedFastKernelPassesOnThisDevice) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    HeatmapGpuBinner::clearSelfTestCacheForTest();
    HeatmapGpuBinner binner(gpu.rhi.get());
    EXPECT_EQ(binner.currentKernel(), KernelVariant::Precise) << "precise until proven";
    uploadPaged(gpu.rhi.get(), binner, smallSource(), 1 << 20); // the self-test waits for an active source
    settleSelfTest(gpu.rhi.get(), binner);
    ASSERT_TRUE(binner.resolvedKernel());
    EXPECT_EQ(*binner.resolvedKernel(), KernelVariant::Fast);
    HeatmapGpuBinner second(gpu.rhi.get()); // cached per device: no second run
    QRhiCommandBuffer *cb = nullptr;
    ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    second.runPrecisionSelfTest(cb);
    ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    EXPECT_EQ(second.resolvedKernel(), std::optional<KernelVariant>(KernelVariant::Fast));
}
TEST(HeatmapGpuSelfTest, FoldingKernelIsRejectedAndPreciseKernelIsExact) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    // The unguarded candidate really is wrong on the fixture...
    const auto fixture = makePrecisionSelfTest();
    HeatmapGpuBinner direct(gpu.rhi.get());
    direct.setFastKernelShaderForTest(QStringLiteral(":/testshaders/heatmap_bin_unguarded.comp.qsb"));
    direct.forceKernel(KernelVariant::Fast);
    uploadPaged(gpu.rhi.get(), direct, std::make_shared<const GpuSource>(fixture.source), 1 << 20);
    const size_t folded = countSelfTestMismatches(fixture, binAndRead(gpu.rhi.get(), direct, fixture.grid));
    std::cout << "unguarded kernel self-test mismatches: " << folded << '/' << fixture.expected.size() << '\n';
    EXPECT_GT(folded, 50u);
    // ...so the self-test rejects it and the binner stays on the precise kernel.
    HeatmapGpuBinner binner(gpu.rhi.get());
    binner.setFastKernelShaderForTest(QStringLiteral(":/testshaders/heatmap_bin_unguarded.comp.qsb"));
    uploadPaged(gpu.rhi.get(), binner, smallSource(), 1 << 20);
    settleSelfTest(gpu.rhi.get(), binner);
    ASSERT_TRUE(binner.resolvedKernel());
    EXPECT_EQ(*binner.resolvedKernel(), KernelVariant::Precise);
    // The precise kernel is exact on the self-test fixture and the stress-style data.
    uploadPaged(gpu.rhi.get(), binner, std::make_shared<const GpuSource>(fixture.source), 1 << 20);
    EXPECT_EQ(countSelfTestMismatches(fixture, binAndRead(gpu.rhi.get(), binner, fixture.grid)), 0u);
    const auto data = compose(minuteLevel(), 5 * minute);
    auto source = std::make_shared<const GpuSource>(buildGpuSource(data));
    uploadPaged(gpu.rhi.get(), binner, source, 1 << 20);
    Tally tally;
    for (const double tick : {5.0, 10.0, 50.0}) {
        const auto grid = gridFor(*source, tick, 99'900, uint32_t(250 / tick) + 2, 3);
        tally.add(compareGrid(data, *source, grid, binAndRead(gpu.rhi.get(), binner, grid), "precise"));
    }
    std::cout << "precise kernel parity: " << tally << '\n';
    EXPECT_TRUE(tally.exact());
}

TEST(HeatmapGpuSelfTest, WaitsForAnActiveSourceAndAWorkerBuiltFixture) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    HeatmapGpuBinner::clearSelfTestCacheForTest();
    HeatmapGpuBinner binner(gpu.rhi.get()); // constructing it starts the worker build
    for (int i = 0; i < 5; ++i) {
        QRhiCommandBuffer *cb = nullptr;
        ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        binner.runPrecisionSelfTest(cb);
        ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    }
    EXPECT_FALSE(binner.resolvedKernel()) << "no active source: nothing to prove yet";
    // The fixture is produced by the worker without any render-thread call.
    std::shared_ptr<const PrecisionSelfTest> fixture;
    for (int i = 0; i < 500 && !fixture; ++i) {
        fixture = precisionSelfTestIfReady();
        if (!fixture) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(fixture);
    EXPECT_EQ(fixture, precisionSelfTestIfReady()) << "built once per process";
    uploadPaged(gpu.rhi.get(), binner, smallSource(), 1 << 20);
    settleSelfTest(gpu.rhi.get(), binner);
    EXPECT_EQ(binner.resolvedKernel(), std::optional<KernelVariant>(KernelVariant::Fast));
}

// FM-099: closing the lab right after its first frame destroyed the render node
// (binner, self-test and its QRhiReadbackResult) before the frame that recorded
// the readback completed; ~QRhi then wrote into the freed result
// (QRhiMetal::finishActiveReadbacks, EXC_BAD_ACCESS). The binner must complete
// an in-flight self-test readback before freeing it. The destruction here happens
// inside the recording frame, the same order as window teardown.
TEST(HeatmapGpuSelfTest, DestroyingTheBinnerWithTheReadbackInFlightCompletesItFirst) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    HeatmapGpuBinner::clearSelfTestCacheForTest();
    auto binner = std::make_unique<HeatmapGpuBinner>(gpu.rhi.get());
    for (int i = 0; i < 500 && !precisionSelfTestIfReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(precisionSelfTestIfReady());
    uploadPaged(gpu.rhi.get(), *binner, smallSource(), 1 << 20);
    const uint64_t drained = HeatmapGpuBinner::drainedSelfTestReadbacksForTest();
    QRhiCommandBuffer *cb = nullptr;
    ASSERT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    binner->runPrecisionSelfTest(cb);
    ASSERT_TRUE(binner->selfTestInFlightForTest()) << "the readback is recorded, not yet completed";
    binner.reset();
    EXPECT_EQ(HeatmapGpuBinner::drainedSelfTestReadbacksForTest(), drained + 1);
    ASSERT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    gpu.rhi.reset(); // ~QRhi finishes no stale readback
    // A binner destroyed with nothing in flight does not stall the GPU.
    Headless again;
    { HeatmapGpuBinner idle(again.rhi.get()); }
    EXPECT_EQ(HeatmapGpuBinner::drainedSelfTestReadbacksForTest(), drained + 1);
}

TEST(HeatmapGpuParity, AllocationFailureBacksOffInsteadOfRetryingEveryFrame) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    HeatmapGpuBinner binner(gpu.rhi.get());
    auto a = smallSource();
    uploadPaged(gpu.rhi.get(), binner, a, 1 << 20);
    auto b = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), minute)));
    binner.setInitialRetryBackoffForTest(std::chrono::milliseconds(150));
    binner.setAllocationFailureForTest(true);
    QString error;
    auto step = [&]() {
        QRhiCommandBuffer *cb = nullptr;
        EXPECT_EQ(gpu.rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        const bool ok = binner.uploadStep(cb, 1 << 20, &error);
        EXPECT_EQ(gpu.rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        return ok;
    };
    ASSERT_TRUE(binner.setSource(b, &error));
    EXPECT_FALSE(step()) << "allocation fails";
    EXPECT_EQ(binner.refusedSourceId(), b->id);
    EXPECT_EQ(binner.retryBackoff(), std::chrono::milliseconds(150));
    EXPECT_EQ(binner.activeSource(), a) << "the active source is untouched";
    // Requested again every frame: no retry until the backoff elapses.
    binner.setAllocationFailureForTest(false);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(binner.setSource(b, &error));
        EXPECT_FALSE(binner.uploadPending());
        EXPECT_TRUE(step());
    }
    EXPECT_EQ(binner.activeSource(), a);
    // After the backoff it retries; a second failure doubles the backoff.
    binner.setAllocationFailureForTest(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));
    ASSERT_TRUE(binner.setSource(b, &error));
    EXPECT_TRUE(binner.uploadPending());
    EXPECT_FALSE(step());
    EXPECT_EQ(binner.retryBackoff(), std::chrono::milliseconds(300));
    // Once allocation works again and the backoff elapsed, the source lands.
    binner.setAllocationFailureForTest(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    uploadPaged(gpu.rhi.get(), binner, b, 1 << 20);
    EXPECT_EQ(binner.activeSource(), b);
    EXPECT_EQ(binner.refusedSourceId(), 0u);
}

TEST(HeatmapGpuParity, MemoryCapRefusesSourceCleanly) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    auto a = smallSource();
    auto big = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), minute)));
    HeatmapGpuBinner binner(gpu.rhi.get(), a->bytes() * 3); // room for the small one only
    uploadPaged(gpu.rhi.get(), binner, a, 1 << 20);
    QString error;
    EXPECT_FALSE(binner.setSource(big, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("cap"))) << error.toStdString();
    EXPECT_FALSE(binner.uploadPending());
    EXPECT_EQ(binner.refusedSourceId(), big->id);
    error.clear();
    EXPECT_TRUE(binner.setSource(big, &error)) << "reported once; a repeat request is a silent no-op";
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(binner.activeSource(), a);
    EXPECT_FALSE(binAndRead(gpu.rhi.get(), binner, gridFor(*a, 10, 99'990, 8, 0)).empty());
    // Raising the cap lifts the refusal.
    binner.setMemoryCap(HeatmapGpuBinner::kDefaultMemoryCapBytes);
    uploadPaged(gpu.rhi.get(), binner, big, 1 << 20);
    EXPECT_EQ(binner.activeSource(), big);
}

TEST(HeatmapGpuParity, RealRecordingOptIn) {
    if (qgetenv("SENTINEL_HEATMAP_REAL_PARITY") != "1")
        GTEST_SKIP() << "set SENTINEL_HEATMAP_REAL_PARITY=1 to compare against the real recording";
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    const std::filesystem::path root("/Volumes/T7/sentinel-data/recording");
    if (!std::filesystem::exists(root / "BTC-USD")) GTEST_SKIP() << "recording directory absent";
    recording::Hmc2Reader reader(root); // read-only; never takes the writer lock
    // Closed past buckets only: the previous complete UTC day is immutable.
    const int64_t end = QDateTime::currentMSecsSinceEpoch() / day * day;
    const int64_t start = end - day;
    struct Case { std::string layer; int64_t tf; };
    Tally total;
    for (const Case c : {Case{"deep", minute}, Case{"deep", 5 * minute}, Case{"deep", hour}, Case{"near", minute}}) {
        SparseColumns data;
        try {
            data = compose(loadRecordingLevels(reader, "BTC-USD", c.layer, start, end, c.tf), c.tf);
        } catch (const std::exception &e) {
            std::cout << "real " << c.layer << " " << c.tf / minute << "m: skipped (" << e.what() << ")\n";
            continue;
        }
        if (data.columns.empty()) continue;
        auto source = std::make_shared<const GpuSource>(buildGpuSource(data));
        HeatmapGpuBinner binner(gpu.rhi.get());
        uploadPaged(gpu.rhi.get(), binner, source, 64ull << 20);
        // Centre on the median entry price: entries are densest around the market.
        std::vector<double> prices;
        for (size_t i = 0; i < data.columns.size(); i += 7)
            for (const auto &n : data.columns[i].native)
                for (size_t e = 0; e < n.entries.size(); e += 13)
                    prices.push_back(double(n.baseRow + n.entries[e].row()) * n.grid.rowTickUnits / n.grid.priceScale);
        ASSERT_FALSE(prices.empty());
        std::nth_element(prices.begin(), prices.begin() + prices.size() / 2, prices.end());
        const double mid = prices[prices.size() / 2];
        std::cout << "real " << c.layer << " source: columns=" << source->columns() << " ticks=" << source->ticks.size()
                  << " first_tick=" << source->ticks.front() << " covered=[" << source->coveredPriceLo << ", "
                  << source->coveredPriceHi << ") wide=" << source->wide << " bytes=" << source->bytes() << '\n';
        Tally tally;
        for (const double factor : {1.0, 2.0, 10.0, 50.0}) {
            const double tick = source->ticks.front() * factor;
            const auto grid = gridFor(*source, tick, std::max(tick, mid - 400 * tick), 800, 0);
            const auto cells = binAndRead(gpu.rhi.get(), binner, grid);
            tally.add(compareGrid(data, *source, grid, cells, "real " + c.layer));
        }
        std::cout << "real " << c.layer << " " << c.tf / minute << "m (" << source->entryCount << " entries): "
                  << tally << '\n';
        EXPECT_TRUE(tally.exact()) << c.layer << " " << c.tf;
        total.add(tally);
    }
    std::cout << "real GPU parity total: " << total << '\n';
    EXPECT_GT(total.valid, 0u);
}

// ---------------------------------------------------------------- scene graph
class HeatmapTestItem final : public QQuickItem {
public:
    std::shared_ptr<HeatmapRenderStats> stats = std::make_shared<HeatmapRenderStats>();
    std::shared_ptr<const GpuSource> source;
    ViewWindow view;
    double tick = 10;              // Manual tick unless autoTick
    bool autoTick = false;
    double hysteresis = 0.25, crossfadeMs = 0;
    recording::SizeScale scale;
    uint64_t cap = HeatmapGpuBinner::kDefaultMemoryCapBytes;
    HeatmapTestItem() { setFlag(ItemHasContents, true); }
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        auto *node = old ? static_cast<HeatmapRenderNode *>(old) : new HeatmapRenderNode(stats);
        HeatmapRenderNode::Frame frame;
        frame.source = source;
        frame.view = view;
        frame.tick.mode = autoTick ? heatmap::TickMode::Auto : heatmap::TickMode::Manual;
        frame.tick.manualTick = tick;
        frame.tick.hysteresis = hysteresis;
        frame.tick.crossfadeMs = crossfadeMs;
        frame.tick.heightPx = height() * window()->effectiveDevicePixelRatio();
        frame.outputScale = scale;
        frame.gpuMemoryCapBytes = cap;
        frame.rect = QRectF(0, 0, width(), height());
        node->setFrame(frame);
        return node;
    }
};

// 10 one-minute buckets: 0-1 before oldest (no data), 2-3 not loaded, 4 a
// recorder gap, 5-9 present. Price bin [100000, 100010) holds a large bid.
std::shared_ptr<const GpuSource> stateSource() {
    SparseColumns data{"BTC-USD", "deep", minute, epoch + 2 * minute, epoch + 10 * minute, {},
                       {{epoch + 4 * minute, epoch + 10 * minute}}};
    for (int64_t i = 5; i < 10; ++i) {
        NativeColumn n;
        n.grid = {1, 1000, 100}; // $10
        n.observedMs = minute;
        n.baseRow = 9'990;
        n.coverage[0] = {{9'990, 10'009, uint64_t(minute)}};
        n.coverage[1] = {{9'990, 10'009, uint64_t(minute)}};
        n.entries = {{packRowSide(10'000, n.baseRow, false), recording::encodeSize(5000)}};
        data.columns.push_back({epoch + i * minute, uint64_t(minute), 0, {n}});
    }
    GpuSourceOptions options;
    options.availableStartMs = epoch + 2 * minute;
    options.availableEndMs = epoch + 10 * minute;
    return std::make_shared<const GpuSource>(buildGpuSource(compose(data, minute), options));
}

TEST(HeatmapRenderNodeScene, DrawsFourStatesAndPansWithoutRebinning) {
    if (!lab::metalDeviceAvailable()) GTEST_SKIP() << "No MTLDevice";
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(200, 100), &error)) << error.toStdString();
    scene.window()->setColor(Qt::black);
    auto *item = new HeatmapTestItem;
    item->setParentItem(scene.window()->contentItem());
    item->setSize(QSizeF(200, 100));
    item->source = stateSource();
    // 10 buckets across 200 px (20 px each); price 99,950..100,050 over 100 px (10 bins).
    item->view = {double(epoch), double(epoch + 10 * minute), 99'950, 100'050};
    item->update();
    QImage frame = scene.renderFrame(&error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    // The precision self-test resolves within a frame or two (or is cached);
    // resolving to the fast kernel re-bins once. Count rebins after that.
    for (int i = 0; i < 3; ++i) { item->update(); frame = scene.renderFrame(&error); }
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    ASSERT_EQ(item->stats->errors.load(), 0u);
    const uint64_t baseRebins = item->stats->rebins.load();
    ASSERT_GE(baseRebins, 1u);
    // Bin [100000, 100010) is rows y in [40, 50).
    const int y = 45;
    auto at = [&](int x, int yy = y) { return frame.pixelColor(x, yy); };
    EXPECT_EQ(at(10), QColor(Qt::black)) << "no data draws nothing";
    const QColor loadA = at(50), loadB = at(55); // 5 px diagonal stripes
    EXPECT_NE(loadA, loadB) << "loading is a hatch, not a flat fill";
    EXPECT_NE(loadA, QColor(Qt::black));
    const QColor veil = at(90);
    EXPECT_NE(veil, QColor(Qt::black));
    EXPECT_EQ(veil, at(95));
    EXPECT_NE(veil, loadA);
    EXPECT_NE(veil, loadB);
    const QColor data = at(150);
    EXPECT_GT(data.green(), 100) << "large bid in the bid palette";
    EXPECT_EQ(at(150, 5), QColor(Qt::black)) << "valid empty cell above the band draws nothing";
    // Pan by 0.3 bucket (6 px) and 0.4 bin: translation only, no compute pass.
    item->view.timeLoMs += 0.3 * minute; item->view.timeHiMs += 0.3 * minute;
    item->update();
    QImage panned = scene.renderFrame(&error);
    ASSERT_FALSE(panned.isNull());
    EXPECT_EQ(item->stats->rebins.load(), baseRebins);
    EXPECT_EQ(panned.pixelColor(150 - 6, y), data);
    EXPECT_EQ(panned.pixelColor(99 - 6, y), frame.pixelColor(99, y));   // veil/data edge moved 6 px
    EXPECT_EQ(panned.pixelColor(100 - 6, y), frame.pixelColor(100, y));
    EXPECT_NE(panned.pixelColor(99 - 6, y), panned.pixelColor(100 - 6, y));
    // A source over the GPU memory cap is refused once (not every frame), and
    // the active picture keeps drawing.
    {
        const uint64_t errorsBefore = item->stats->errors.load();
        const auto good = item->source;
        auto big = std::make_shared<const GpuSource>(buildGpuSource(compose(minuteLevel(), minute)));
        item->source = big;
        item->cap = good->bytes() * 4;
        QImage kept;
        for (int i = 0; i < 5; ++i) { item->update(); kept = scene.renderFrame(&error); }
        ASSERT_FALSE(kept.isNull());
        EXPECT_EQ(item->stats->errors.load(), errorsBefore + 1) << "reported once, not per frame";
        EXPECT_EQ(item->stats->refusedSourceId.load(), big->id);
        EXPECT_EQ(kept.pixelColor(150 - 6, y), data);
        item->source = good;
        item->cap = HeatmapGpuBinner::kDefaultMemoryCapBytes;
    }
    // Zooming the price axis changes the display tick: a new compute pass.
    item->tick = 20;
    item->update();
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_EQ(item->stats->rebins.load(), baseRebins + 1);
    // A new output size scale changes every code: it must re-bin (the view did not move).
    item->scale = {1e-8, 819};
    item->update();
    const QImage rescaled = scene.renderFrame(&error);
    ASSERT_FALSE(rescaled.isNull());
    EXPECT_EQ(item->stats->rebins.load(), baseRebins + 2);
    item->update();
    ASSERT_FALSE(scene.renderFrame(&error).isNull());
    EXPECT_EQ(item->stats->rebins.load(), baseRebins + 2) << "same scale again: no re-bin";
}

// ---------------------------------------------------------------- slice T
// Columns of `data` in [startMs, endMs): a chunk of the same recording.
SparseColumns sliceOf(const SparseColumns &data, int64_t startMs, int64_t endMs) {
    SparseColumns out{data.symbol, data.layer, data.tfMs, startMs, endMs, {}, {}};
    for (const auto &c : data.columns)
        if (c.bucketStartMs >= startMs && c.bucketStartMs < endMs) out.columns.push_back(c);
    for (const auto &r : data.scannedRanges) {
        const int64_t a = std::max(r.startMs, startMs), b = std::min(r.endMs, endMs);
        if (a < b) out.scannedRanges.push_back({a, b});
    }
    validate(out);
    return out;
}

// Spec rule 3: a (tick, price) and a (timeframe, time) always map to the same
// cell, whatever the view, the grid margin, a reload (new source id) or the
// chunk the columns arrived in. Every cell two readbacks share must be equal.
TEST(HeatmapGpuAnchoring, CellsKeepAbsolutePriceAndTimeAcrossViewsReloadsAndChunks) {
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    const auto minutes = minuteLevel();
    for (const int64_t tf : {5 * minute, 16 * minute}) {
        const auto data = compose(minutes, tf);
        // The chunk starts on a bucket boundary of its timeframe: 06:00 for 5 m,
        // 13:04 (49 x 16 m) for 16 m.
        const int64_t cut = tf == 16 * minute ? 784 * minute : 6 * hour;
        struct Variant { std::string label; std::shared_ptr<const GpuSource> source; int64_t fromMs; };
        std::vector<Variant> variants{
            {"full", std::make_shared<const GpuSource>(buildGpuSource(data)), data.startMs},
            {"reload", std::make_shared<const GpuSource>(buildGpuSource(data)), data.startMs},
            {"chunk", std::make_shared<const GpuSource>(buildGpuSource(sliceOf(data, epoch + cut, data.endMs))), epoch + cut}};
        ASSERT_NE(variants[0].source->id, variants[1].source->id);
        // Views at different fractional offsets, spans and margins.
        std::vector<std::pair<ViewWindow, int64_t>> views;
        for (int i = 0; i < 6; ++i) {
            const double t0 = double(epoch + 10 * hour) + i * 0.37 * double(tf) - i * 3 * double(tf);
            const double p0 = 99'950 + i * 3.3;
            views.push_back({{t0, t0 + (40 + 7 * i) * double(tf), p0, p0 + 60 + 11 * i}, int64_t(i % 3)});
        }
        uint64_t validAll = 0;
        for (const double tick : {10.0, 20.0, 5.0}) {
            // (absolute bucket, absolute bin) -> cell, from the first variant/view that bins it.
            std::map<std::pair<int64_t, int64_t>, uint32_t> seen;
            uint64_t compared = 0, mismatches = 0, valid = 0;
            for (const auto &v : variants) {
                HeatmapGpuBinner binner(gpu.rhi.get());
                uploadPaged(gpu.rhi.get(), binner, v.source, 64ull << 20);
                for (const auto &[view, margin] : views) {
                    const auto grid = planGrid(view, tf, tick, margin);
                    ASSERT_TRUE(grid);
                    const auto cells = binAndRead(gpu.rhi.get(), binner, *grid);
                    for (uint32_t x = 0; x < grid->columns; ++x) {
                        const int64_t bucket = grid->firstBucket + x;
                        if (bucket * tf < v.fromMs) continue; // before this chunk: legitimately not loaded
                        for (uint32_t y = 0; y < grid->rows; ++y) {
                            const int64_t bin = grid->firstBin + int64_t(grid->rows - 1 - y);
                            const uint32_t cell = cells[size_t(y) * grid->columns + x];
                            valid += decodeCell(cell).state == CellState::Valid;
                            const auto [it, inserted] = seen.emplace(std::pair(bucket, bin), cell);
                            if (inserted) continue;
                            ++compared;
                            if (it->second != cell && mismatches++ < 5)
                                std::cerr << v.label << " tf=" << tf / minute << "m tick=" << tick << " bucket="
                                          << bucket << " bin=" << bin << " moved: " << it->second << " vs " << cell
                                          << '\n';
                        }
                    }
                }
            }
            std::cout << "anchoring tf=" << tf / minute << "m tick=" << tick << ": compared=" << compared
                      << " valid=" << valid << " mismatches=" << mismatches << '\n';
            EXPECT_EQ(mismatches, 0u);
            EXPECT_GT(compared, 5'000u);
            validAll += valid;
        }
        EXPECT_GT(validAll, 1'000u) << "the comparison covers real data, not only empty cells";
    }
}

// Manual never coarsens: a locked tick is drawn over columns that cannot build
// it; Auto skips to the finest preset every column in view can build.
TEST(HeatmapTickPolicyNode, ManualKeepsItsTickAutoFollowsTheDataInView) {
    const auto source = buildGpuSource(compose(minuteLevel(), minute)); // $10 until 06:00, then $5
    HeatmapRenderNode::TickPolicy manual;
    manual.mode = heatmap::TickMode::Manual;
    manual.manualTick = 5;
    manual.heightPx = 100;
    const ViewWindow both{double(epoch + 5 * hour), double(epoch + 7 * hour), 99'900, 100'100};
    const ViewWindow recent{double(epoch + 7 * hour), double(epoch + 9 * hour), 99'900, 100'100};
    EXPECT_EQ(HeatmapRenderNode::displayTickFor(source, both, manual), 5);
    EXPECT_EQ(HeatmapRenderNode::displayTickFor(source, recent, manual), 5);
    manual.manualTick = 3; // not a preset
    EXPECT_EQ(HeatmapRenderNode::displayTickFor(source, both, manual), 0);
    EXPECT_DOUBLE_EQ(commonTickInView(source, both.timeLoMs, both.timeHiMs), 10);
    EXPECT_DOUBLE_EQ(commonTickInView(source, recent.timeLoMs, recent.timeHiMs), 5);
    EXPECT_EQ(columnCommonTicks(source), (std::vector<double>{5, 10}));
    HeatmapRenderNode::TickPolicy autoPolicy;
    autoPolicy.heightPx = 400; // $200 over 400 px: $1 rows 2 px; $5 is the finest buildable
    EXPECT_EQ(HeatmapRenderNode::displayTickFor(source, both, autoPolicy), 10);
    EXPECT_EQ(HeatmapRenderNode::displayTickFor(source, recent, autoPolicy), 5);
    // $5 over the view that straddles the grid change: 60 of 120 columns cannot build it.
    const auto coverage = tickCoverage(source, (epoch + 5 * hour) / minute, (epoch + 7 * hour) / minute, 5);
    EXPECT_GT(coverage.incompatible, 0u);
    EXPECT_EQ(coverage.endIncompatibleBucket, (epoch + 6 * hour) / minute);
    EXPECT_DOUBLE_EQ(coverage.incompatibleCommon, 10);
    EXPECT_EQ(tickCoverage(source, (epoch + 5 * hour) / minute, (epoch + 7 * hour) / minute, 10).incompatible, 0u);
    // The veil itself: $5 over a $10 column reads back as veil (GPU).
    Headless gpu;
    if (!gpu.rhi) GTEST_SKIP() << "No MTLDevice";
    auto shared = std::make_shared<const GpuSource>(source);
    HeatmapGpuBinner binner(gpu.rhi.get());
    uploadPaged(gpu.rhi.get(), binner, shared, 64ull << 20);
    const auto grid = planGrid(both, minute, 5, 0);
    ASSERT_TRUE(grid);
    const auto cells = binAndRead(gpu.rhi.get(), binner, *grid);
    uint64_t oldVeil = 0, oldValid = 0, newValid = 0;
    for (uint32_t x = 0; x < grid->columns; ++x)
        for (uint32_t y = 0; y < grid->rows; ++y) {
            const auto state = decodeCell(cells[size_t(y) * grid->columns + x]).state;
            const bool old = (grid->firstBucket + x) * minute < epoch + 6 * hour;
            if (old) { oldVeil += state == CellState::Veil; oldValid += state == CellState::Valid; }
            else newValid += state == CellState::Valid;
        }
    EXPECT_EQ(oldValid, 0u) << "a $10 column never draws a $5 row";
    EXPECT_GT(oldVeil, 0u);
    EXPECT_GT(newValid, 0u);
}

// Auto in the real scene graph: pans inside the prepared grid never re-bin;
// the tick changes only past the hysteresis thresholds, once per crossing;
// jitter around a threshold never flips it; a crossfade costs no extra bin.
TEST(HeatmapRenderNodeScene, AutoTickHysteresisAndPansNeverRebin) {
    if (!lab::metalDeviceAvailable()) GTEST_SKIP() << "No MTLDevice";
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(200, 100), &error)) << error.toStdString();
    auto *item = new HeatmapTestItem;
    item->setParentItem(scene.window()->contentItem());
    item->setSize(QSizeF(200, 100));
    item->source = stateSource(); // $10 grid
    item->autoTick = true;
    const double dpr = scene.window()->effectiveDevicePixelRatio();
    const double heightPx = 100 * dpr;
    // $10 rows `rowPx` physical pixels tall, centred on 100,005.
    auto zoomTo = [&](double rowPx) {
        const double span = heightPx * 10 / rowPx;
        item->view.priceLo = 100'005 - span / 2;
        item->view.priceHi = 100'005 + span / 2;
    };
    auto frame = [&] {
        item->update();
        const QImage image = scene.renderFrame(&error);
        EXPECT_FALSE(image.isNull()) << error.toStdString();
        return image;
    };
    item->view.timeLoMs = double(epoch);
    item->view.timeHiMs = double(epoch + 10 * minute);
    zoomTo(2.2);
    for (int i = 0; i < 4; ++i) frame(); // settle the kernel self-test
    ASSERT_EQ(item->stats->errors.load(), 0u);
    EXPECT_EQ(item->stats->tick.load(), 10);
    EXPECT_EQ(item->stats->commonTick.load(), 10);
    const uint64_t rebins = item->stats->rebins.load(), changes = item->stats->tickChanges.load();
    // Pure pans (time and price, sub-bin and up to one bin) inside the prepared grid.
    for (int i = 0; i < 12; ++i) {
        const double dt = (i % 2 ? -0.45 : 0.4) * minute, dp = (i % 3 ? 3.0 : -4.0);
        item->view.timeLoMs += dt; item->view.timeHiMs += dt;
        item->view.priceLo += dp; item->view.priceHi += dp;
        frame();
    }
    EXPECT_EQ(item->stats->rebins.load(), rebins) << "a pan inside the prepared region is a pure translation";
    EXPECT_EQ(item->stats->tickChanges.load(), changes);
    // Zoom out inside the hysteresis band (h = 0.25: coarser below 1.5 px).
    zoomTo(1.6); frame();
    EXPECT_EQ(item->stats->tick.load(), 10);
    zoomTo(1.4); frame();
    EXPECT_EQ(item->stats->tick.load(), 20);
    EXPECT_EQ(item->stats->tickChanges.load(), changes + 1);
    // Back in: finer only once $10 rows reach 2.5 px.
    zoomTo(2.4); frame();
    EXPECT_EQ(item->stats->tick.load(), 20);
    zoomTo(2.6); frame();
    EXPECT_EQ(item->stats->tick.load(), 10);
    EXPECT_EQ(item->stats->tickChanges.load(), changes + 2);
    // Jitter around the threshold just crossed: no flip.
    for (int i = 0; i < 10; ++i) { zoomTo(i % 2 ? 2.6 : 2.4); frame(); }
    EXPECT_EQ(item->stats->tickChanges.load(), changes + 2);
    // Same zoom, pans again: still no re-bin.
    const uint64_t before = item->stats->rebins.load();
    for (int i = 0; i < 6; ++i) {
        item->view.timeLoMs += 0.3 * minute; item->view.timeHiMs += 0.3 * minute;
        frame();
        item->view.timeLoMs -= 0.3 * minute; item->view.timeHiMs -= 0.3 * minute;
        frame();
    }
    EXPECT_EQ(item->stats->rebins.load(), before);
    // Crossfade: one compute pass per tick change, the old grid kept for the fade.
    item->crossfadeMs = 60'000;
    zoomTo(1.4); frame();
    EXPECT_EQ(item->stats->rebins.load(), before + 1);
    EXPECT_EQ(item->stats->tick.load(), 20);
    EXPECT_TRUE(item->stats->crossfading.load());
    frame();
    EXPECT_TRUE(item->stats->crossfading.load());
    EXPECT_EQ(item->stats->rebins.load(), before + 1) << "fading frames do not re-bin";
    item->crossfadeMs = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    frame();
    EXPECT_FALSE(item->stats->crossfading.load());
    EXPECT_EQ(item->stats->errors.load(), 0u);
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
