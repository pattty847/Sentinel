#include "HeatmapGpuSelfTest.hpp"
#include "heatmap/BinCell.hpp"
#include <cmath>
#include <future>
#include <mutex>
#include <random>

namespace heatmap::gpu {
namespace {
constexpr int64_t kEpoch = 946'684'800'000LL + 20 * kDayMs; // 2000-01-21 UTC, any day-aligned time
constexpr int64_t kFirstBin = 12'500;  // $100,000 at an $8 display tick
constexpr uint32_t kRows = 64;
constexpr int64_t kFactor = 8;         // $1 native rows per $8 bin
constexpr uint64_t kObservedMs = uint64_t(kMinuteMs);

NativeColumn emptyNative() {
    NativeColumn n;
    n.grid = {99, 100, 100}; // $1
    n.observedMs = kObservedMs;
    n.composed = true;
    n.baseRow = kFirstBin * kFactor;
    const int64_t hi = (kFirstBin + kRows) * kFactor - 1;
    n.coverage[0] = {{n.baseRow, hi, kObservedMs}};
    n.coverage[1] = {{n.baseRow, hi, kObservedMs}};
    return n;
}
void push(NativeColumn &n, int64_t row, bool ask, double value) {
    n.entries.push_back({packRowSide(row, n.baseRow, ask), recording::encodeSize(value)});
    n.numerators.push_back(static_cast<long double>(value) * kObservedMs);
}
} // namespace

PrecisionSelfTest makePrecisionSelfTest() {
    PrecisionSelfTest test;
    auto &data = test.data;
    data.symbol = "SELFTEST";
    data.layer = "near";
    data.tfMs = kMinuteMs;
    const auto &thresholds = cachedEncodeThresholds({});
    // Columns 0 (bid) and 1 (ask): adversarial threshold-crossing bins.
    for (int side = 0; side < 2; ++side) {
        NativeColumn n = emptyNative();
        for (uint32_t j = 0; j < kRows; ++j) {
            const uint32_t k = 2000 + j * 400;
            const double t = double(thresholds[k - 2].hi) + double(thresholds[k - 2].lo);
            float a = static_cast<float>(t);
            if (double(a) >= t) a = std::nextafter(a, 0.0f); // largest float below t
            const double d = t - double(a);                   // 0 < d <= ulp(a)
            const double b = (d + t * 1e-9) / 4;              // each < ulp(a)/2: lost by a plain float add
            const int64_t row = (kFirstBin + j) * kFactor;
            push(n, row, side == 1, double(a));
            for (int r = 1; r <= 4; ++r) push(n, row + r, side == 1, b);
        }
        data.columns.push_back({kEpoch + side * kMinuteMs, kObservedMs, 0, {std::move(n)}});
    }
    // Columns 2..5: ordinary dense random bins on both sides.
    std::mt19937_64 rng(20260929);
    std::uniform_int_distribution<int> code(1, 26'000);
    std::uniform_real_distribution<double> scale(0.3, 3.0);
    for (int c = 2; c < 6; ++c) {
        NativeColumn n = emptyNative();
        for (int64_t row = n.baseRow; row < (kFirstBin + kRows) * kFactor; ++row)
            for (bool ask : {false, true})
                if (rng() % 10 < 7) push(n, row, ask, recording::decodeSize(uint16_t(code(rng))) * scale(rng));
        data.columns.push_back({kEpoch + c * kMinuteMs, kObservedMs, 0, {std::move(n)}});
    }
    data.startMs = kEpoch;
    data.endMs = kEpoch + 6 * kMinuteMs;
    data.scannedRanges = {{data.startMs, data.endMs}};
    validate(data);
    test.source = buildGpuSource(data);
    test.grid = {kMinuteMs, kEpoch / kMinuteMs, 6, double(kFactor), kFirstBin, kRows};
    test.expected.assign(size_t(test.grid.columns) * kRows, 0);
    for (uint32_t x = 0; x < test.grid.columns; ++x) {
        const auto cells = binColumn(data.columns[x], double(kFirstBin * kFactor),
                                     double((kFirstBin + kRows) * kFactor), double(kFactor));
        for (uint32_t y = 0; y < kRows; ++y)
            test.expected[size_t(y) * test.grid.columns + x] =
                (cells[y].code & recording::kMaxCode) | (cells[y].dominantAsk ? 0x8000u : 0u) |
                ((cells[y].valid ? 3u : 2u) << 16);
    }
    return test;
}

namespace {
std::once_flag fixtureOnce;
std::shared_future<std::shared_ptr<const PrecisionSelfTest>> fixtureFuture;
} // namespace
void prewarmPrecisionSelfTest() {
    std::call_once(fixtureOnce, [] {
        fixtureFuture = std::async(std::launch::async, [] {
            return std::shared_ptr<const PrecisionSelfTest>(std::make_shared<PrecisionSelfTest>(makePrecisionSelfTest()));
        }).share();
    });
}
std::shared_ptr<const PrecisionSelfTest> precisionSelfTestIfReady() {
    prewarmPrecisionSelfTest();
    if (fixtureFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    return fixtureFuture.get();
}

size_t countSelfTestMismatches(const PrecisionSelfTest &test, const std::vector<uint32_t> &cells) {
    if (cells.size() != test.expected.size()) return test.expected.size();
    size_t mismatches = 0;
    for (size_t i = 0; i < cells.size(); ++i) mismatches += cells[i] != test.expected[i];
    return mismatches;
}
} // namespace heatmap::gpu
