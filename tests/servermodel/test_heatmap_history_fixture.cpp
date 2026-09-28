#include <gtest/gtest.h>

#include "HeatmapHistoryFixture.hpp"
#include "config/ConfigTypes.hpp"
#include "servermodel/HeatmapTwapStreamer.hpp"
#include "servermodel/IHeatmapDataSource.hpp"

#include <QCoreApplication>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <vector>

namespace fs = std::filesystem;
using namespace heatmap_fixture;

namespace {

constexpr int kPageColumns = 1024;
constexpr int64_t kDayMs = 86'400'000;

QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "test_heatmap_history_fixture";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

class IdleSource : public IHeatmapDataSource {
public:
    IdleSource() : m_hot("BTC-USD") {}
    int64_t exchangeNowMs() const override { return 0; }
    std::vector<std::string> getSymbolsSnapshot() const override { return {}; }
    SymbolHotData& ensureSymbol(const std::string&) override { return m_hot; }
private:
    SymbolHotData m_hot;
};

Spec testSpec() {
    Spec s;
    s.gridHeight = 256;
    s.tickSize = 50.0;
    s.midPrice = 100'000.0;
    s.endMs = 20'700 * kDayMs + 13 * 3'600'000;  // 48 h window crosses two UTC midnights
    return s;
}

class HistoryFixture : public ::testing::Test {
protected:
    void SetUp() override {
        app();
        const auto base = fs::temp_directory_path() / "sentinel-fixture-tests";
        fs::create_directories(base);
        m_dir = base / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        m_spec = testSpec();
        HeatmapColumnStoreConfig cfg;
        cfg.fsyncEveryNRecords = 1 << 30;
        cfg.fsyncEveryMs = 1 << 30;
        HeatmapColumnStore store(m_dir, cfg);
        ASSERT_TRUE(store.acquireLock());
        m_seed = seed(store, m_spec);
        for (const auto& b : plan(m_spec)) {
            m_plan.push_back(b);
            if (b.kind != Kind::Missing) m_expected.insert(b.startMs);
        }
    }

    void TearDown() override { fs::remove_all(m_dir); }

    ServerHeatmapConfig streamerConfig() const {
        ServerHeatmapConfig cfg;
        cfg.gridWidth = 8192;
        cfg.gridHeight = m_spec.gridHeight;
        cfg.tickSize = m_spec.tickSize;
        cfg.timeframesMs = {m_spec.timeframeMs};
        cfg.activeTimeframeMs = m_spec.timeframeMs;
        cfg.persistenceEnabled = true;
        cfg.persistenceDir = m_dir.string();
        return cfg;
    }

    // Newest-first page walk, as the client pages left from live.
    std::vector<HeatmapTwapStreamer::HistoryColumn> walk(const HeatmapTwapStreamer& streamer) {
        std::vector<HeatmapTwapStreamer::HistoryColumn> all;
        int64_t endMs = 0;
        for (int page = 0; page < 16; ++page) {
            std::vector<HeatmapTwapStreamer::HistoryColumn> cols;
            int w = 0;
            int h = 0;
            streamer.fetchHistory(m_spec.symbol, m_spec.timeframeMs, endMs, kPageColumns, w, h, cols);
            if (cols.empty()) break;
            EXPECT_LE(static_cast<int>(cols.size()), kPageColumns);
            for (size_t i = 1; i < cols.size(); ++i) {
                EXPECT_LT(cols[i - 1].bucketStartMs, cols[i].bucketStartMs) << "page " << page;
            }
            all.insert(all.begin(), cols.begin(), cols.end());
            endMs = cols.front().bucketStartMs - 1;
        }
        return all;
    }

    void expectMatchesPlan(const std::vector<HeatmapTwapStreamer::HistoryColumn>& cols) {
        std::set<int64_t> seen;
        for (const auto& c : cols) {
            EXPECT_TRUE(seen.insert(c.bucketStartMs).second) << "duplicate " << c.bucketStartMs;
        }
        EXPECT_EQ(seen, m_expected);
        for (int64_t t : m_expected) {
            if (!seen.count(t)) ADD_FAILURE() << "missing bucket " << t;
        }
        for (int64_t t : seen) {
            if (!m_expected.count(t)) ADD_FAILURE() << "unexpected bucket " << t;
        }
    }

    fs::path m_dir;
    Spec m_spec;
    SeedResult m_seed;
    std::vector<Bucket> m_plan;
    std::set<int64_t> m_expected;
};

} // namespace

TEST_F(HistoryFixture, SeedWritesEveryPlannedBucket) {
    const Layout l;
    EXPECT_EQ(m_seed.failed, 0);
    EXPECT_EQ(m_seed.missing, l.gapShortLen + l.gapLongLen);
    EXPECT_EQ(m_seed.zero, l.zeroLen);
    EXPECT_EQ(m_seed.written, m_spec.totalBuckets - m_seed.missing);

    IdleSource source;
    HeatmapTwapStreamer streamer(source, streamerConfig());
    EXPECT_EQ(streamer.oldestPersistedMs(m_spec.symbol, m_spec.timeframeMs), m_plan.front().startMs);
}

TEST_F(HistoryFixture, DiskPageWalkReturnsExactlyTheRecordedBuckets) {
    IdleSource source;
    HeatmapTwapStreamer streamer(source, streamerConfig());
    expectMatchesPlan(walk(streamer));
}

TEST_F(HistoryFixture, PrimedRingToDiskWalkHasNoDuplicatesOrGaps) {
    IdleSource source;
    HeatmapTwapStreamer streamer(source, streamerConfig());
    ASSERT_GT(streamer.bootstrapFromDisk({m_spec.symbol}), 0);
    expectMatchesPlan(walk(streamer));
}

TEST_F(HistoryFixture, ReferenceLineKeepsItsPriceAcrossTheBandShift) {
    IdleSource source;
    HeatmapTwapStreamer streamer(source, streamerConfig());
    const auto cols = walk(streamer);
    ASSERT_FALSE(cols.empty());

    const double ref = referencePrice(m_spec);
    std::set<double> bandMids;
    int zeroColumns = 0;
    for (const auto& c : cols) {
        const auto* cells = reinterpret_cast<const uint16_t*>(c.intensity.constData());
        const int h = static_cast<int>(c.intensity.size() / 2);
        ASSERT_EQ(h, m_spec.gridHeight);
        const bool allZero = std::all_of(cells, cells + h, [](uint16_t v) { return v == 0; });
        if (allZero) {
            ++zeroColumns;
            continue;
        }
        bandMids.insert((c.minPrice + c.maxPrice) * 0.5);
        const int row = static_cast<int>(std::floor((c.maxPrice - ref) / c.tickSize));
        ASSERT_GE(row, 0);
        ASSERT_LT(row, h);
        EXPECT_EQ(cells[row], encodeAsk(1.0)) << "bucket " << c.bucketStartMs;
        const double rowTop = c.maxPrice - row * c.tickSize;
        EXPECT_LE(std::abs(rowTop - ref), c.tickSize);
    }
    EXPECT_EQ(zeroColumns, Layout{}.zeroLen);
    EXPECT_EQ(bandMids.size(), 2u);
}
