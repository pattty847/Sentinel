#include <gtest/gtest.h>

#include "config/ConfigTypes.hpp"
#include "servermodel/HeatmapColumnStore.hpp"
#include "servermodel/HeatmapTwapStreamer.hpp"
#include "servermodel/IHeatmapDataSource.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMs1m = 60'000;
constexpr int64_t kMs1h = 3'600'000;
constexpr int kGridHeight = 64;
const std::string kSymbol = "BTC-USD";

QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "test_heatmap_twap_streamer";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

fs::path makeTempDir(const std::string& tag) {
    const auto base = fs::temp_directory_path() / "sentinel-twap-tests";
    fs::create_directories(base);
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = base / (tag + "-" + std::to_string(t));
    fs::create_directories(p);
    return p;
}

// Scripted exchange clock: each sample tick reads the next time from `clock`.
class FakeSource : public IHeatmapDataSource {
public:
    explicit FakeSource(std::function<int64_t(int)> clockFn)
        : m_clock(std::move(clockFn)), m_hot(kSymbol) {
        m_hot.lastTradePrice = 50'000.0;
    }

    int64_t exchangeNowMs() const override { return m_clock(m_calls++); }
    std::vector<std::string> getSymbolsSnapshot() const override { return {kSymbol}; }
    SymbolHotData& ensureSymbol(const std::string&) override { return m_hot; }

    int calls() const { return m_calls; }

private:
    std::function<int64_t(int)> m_clock;
    mutable int m_calls = 0;
    SymbolHotData m_hot;
};

ServerHeatmapConfig baseConfig() {
    ServerHeatmapConfig cfg;
    cfg.gridWidth = 1024;
    cfg.gridHeight = kGridHeight;
    cfg.timeframesMs = {kMs1m};
    cfg.activeTimeframeMs = kMs1m;
    return cfg;
}

void runUntilCalls(HeatmapTwapStreamer& streamer, const FakeSource& source, int calls) {
    streamer.start();
    QElapsedTimer timer;
    timer.start();
    while (source.calls() < calls && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    streamer.stop();
    ASSERT_GE(source.calls(), calls) << "sample timer did not reach the scripted tick count";
}

std::vector<HeatmapTwapStreamer::HistoryColumn> history(const HeatmapTwapStreamer& streamer) {
    std::vector<HeatmapTwapStreamer::HistoryColumn> out;
    int width = 0;
    int height = 0;
    streamer.fetchHistory(kSymbol, kMs1m, 0, 100'000, width, height, out);
    return out;
}

} // namespace

// A restart must not integrate the current book across the downtime.
TEST(HeatmapTwapStreamerGaps, PrimedRestartLeavesDowntimeMissing) {
    app();
    const auto dir = makeTempDir("restart");
    const int64_t persistedStart = 1'700'000'000'000 / kMs1m * kMs1m;
    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        std::vector<uint16_t> cells(kGridHeight, 1000);
        ASSERT_EQ(store.append(kSymbol, kMs1m, kGridHeight, persistedStart,
                               persistedStart + kMs1m, 49'000.0, 51'000.0, 1.0,
                               cells.data(), cells.data(), 1.0),
                  HeatmapColumnStore::AppendResult::Written);
    }

    const int64_t restartMs = persistedStart + 10 * kMs1h;
    FakeSource source([&](int i) { return restartMs + i * 50; });
    auto cfg = baseConfig();
    cfg.persistenceEnabled = true;
    cfg.persistenceDir = dir.string();
    HeatmapTwapStreamer streamer(source, cfg);
    ASSERT_EQ(streamer.bootstrapFromDisk({kSymbol}), 1);

    runUntilCalls(streamer, source, 5);

    const auto cols = history(streamer);
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols.front().bucketStartMs, persistedStart);
}

// A long sample gap closes the observed partial bucket and fabricates nothing.
TEST(HeatmapTwapStreamerGaps, SleepGapClosesObservedBucketOnly) {
    app();
    const int64_t minute = 1'700'000'000'000 / kMs1m * kMs1m;
    const int64_t wakeMs = minute + 8 * kMs1h;
    FakeSource source([&](int i) {
        if (i < 30) return minute + i * 1000;  // 29 s observed in the first minute
        return wakeMs + (i - 30) * 1000;       // host slept for 8 h
    });
    HeatmapTwapStreamer streamer(source, baseConfig());

    runUntilCalls(streamer, source, 41);

    const auto cols = history(streamer);
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols.front().bucketStartMs, minute);
}

TEST(HeatmapTwapStreamerRollup, MissingBucketsAndSignedMean) {
    app();
    const auto dir = makeTempDir("rollup");
    const int64_t base = (1'700'000'000'000 / 300'000) * 300'000;
    {
        HeatmapColumnStore store(dir);
        ASSERT_TRUE(store.acquireLock());
        std::vector<uint16_t> cells(kGridHeight, 0);
        std::vector<uint16_t> liquidity(kGridHeight, 0);
        for (const auto [minute, encoded, quantity] : {
                 std::tuple<int, uint16_t, uint16_t>{0, 1000, 100},
                 {1, static_cast<uint16_t>(0x8000u + 3000), 300},
                 {10, 2000, 500}}) {
            cells[0] = encoded;
            liquidity[0] = quantity;
            ASSERT_EQ(store.append(kSymbol, kMs1m, kGridHeight, base + minute * kMs1m,
                                   base + (minute + 1) * kMs1m, 0, kGridHeight, 1,
                                   cells.data(), liquidity.data(), 1.0),
                      HeatmapColumnStore::AppendResult::Written);
        }
    }
    FakeSource source([](int) { return 0; });
    auto cfg = baseConfig();
    cfg.timeframesMs = {kMs1m, 300'000};
    cfg.persistenceEnabled = true;
    cfg.persistenceDir = dir.string();
    HeatmapTwapStreamer streamer(source, cfg);
    std::vector<HeatmapTwapStreamer::HistoryColumn> out;
    int width = 0, height = 0;
    ASSERT_TRUE(streamer.fetchHistory(kSymbol, 300'000, 0, 10, width, height, out));
    ASSERT_EQ(out.size(), 2u); // Entire middle 5m bucket is absent.
    EXPECT_EQ(out[0].bucketStartMs, base);
    EXPECT_EQ(out[1].bucketStartMs, base + 600'000);
    EXPECT_EQ(out[0].bucketEndMs, base + 300'000);
    const auto* first = reinterpret_cast<const uint16_t*>(out[0].intensity.constData());
    const auto* second = reinterpret_cast<const uint16_t*>(out[1].intensity.constData());
    EXPECT_EQ(first[0], static_cast<uint16_t>(0x8000u + 1000));
    EXPECT_EQ(second[0], 2000);
    const auto* quantity = reinterpret_cast<const uint16_t*>(out[0].liquidity.constData());
    EXPECT_NEAR(quantity[0] * out[0].liquidityScale, 200.0, 0.01);
    fs::remove_all(dir);
}

TEST(HeatmapTwapStreamerRollup, LiveRolledBucketRefreshesOnMinuteFinalize) {
    app();
    const int64_t base = (1'700'000'000'000 / 300'000) * 300'000;
    FakeSource source([&](int i) { return base + i * 5'000; });
    auto cfg = baseConfig();
    cfg.timeframesMs = {kMs1m, 300'000};
    HeatmapTwapStreamer streamer(source, cfg);
    std::vector<int64_t> rolledStarts;
    QObject::connect(&streamer, &HeatmapTwapStreamer::heatmapSliceReady,
                     &streamer, [&](const HeatmapSlice& slice) {
                         if (slice.timeframeMs == 300'000)
                             rolledStarts.push_back(slice.bucketStartMs);
                     });
    runUntilCalls(streamer, source, 27);
    ASSERT_GE(rolledStarts.size(), 2u);
    EXPECT_EQ(rolledStarts[0], base);
    EXPECT_EQ(rolledStarts[1], base);
}
