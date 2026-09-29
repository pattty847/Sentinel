#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QtEndian>
#include "protocol/SentinelStreamClientParseHelpers.hpp"
#include "servermodel/TradeOverlayPublisher.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "render/TradeOverlayMapping.hpp"
#include "render/FootprintStreamState.hpp"
#include "render/TpoStreamState.hpp"
#include "render/DataProcessor.hpp"

struct TradeOverlayWireTest {
    static void receive(SentinelStreamClient& client, const nlohmann::json& msg) {
        const auto type = msg.at("type");
        if (type == "footprint_slice") client.handleFootprintSliceMessage(msg);
        else if (type == "footprint_history_chunk") client.handleFootprintHistoryChunkMessage(msg);
        else if (type == "tpo_slice") client.handleTpoSliceMessage(msg);
        else if (type == "tpo_history_chunk") client.handleTpoHistoryChunkMessage(msg);
        else if (type == "volume_profile_slice") client.handleVolumeProfileSliceMessage(msg);
    }
};
struct TradeOverlayModelTest {
    static void seed(ServerDataModel& model, const std::string& symbol,
                     std::deque<ServerDataModel::FootprintTradeSample> trades) {
        std::lock_guard<std::mutex> lock(model.m_footprintTradeMutex);
        model.m_recentFootprintTrades[symbol] = std::move(trades);
    }
};
namespace {
using namespace trade_overlay;
constexpr int64_t day = 172800000;
Request request() {
    Request q; q.symbol = "BTC-USD"; q.grid = {16, 10, 2, 120};
    q.nowMs = day + 120001; q.previousMs = day + 119001; q.count = 2;
    return q;
}
std::vector<ServerDataModel::FootprintTradeSample> tape() {
    return {{day + 60000, 115, 3, AggressorSide::Buy},
            {day + 119999, 115, 1, AggressorSide::Sell},
            {day + 120000, 111, 5, AggressorSide::Sell}};
}
class TradeOverlay : public testing::Test {
protected:
    int argc = 1; char name[5] = "test"; char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
};
}
TEST_F(TradeOverlay, CadenceClosesPreviousThenRefreshesFormingWithoutBacklog) {
    EXPECT_EQ(liveBuckets(120001, 119001, 60000), (std::vector<int64_t>{60000,120000}));
    EXPECT_EQ(liveBuckets(121001, 120001, 60000), (std::vector<int64_t>{120000}));
    EXPECT_EQ(liveBuckets(600001, 120001, 60000), (std::vector<int64_t>{540000,600000}));
    EXPECT_EQ(liveBuckets(180001, 0, 60000, 30000), (std::vector<int64_t>{150000}));
    EXPECT_EQ(kRefreshMs, 1000);
}
TEST_F(TradeOverlay, WireRoundTripRetainsOwnGridAndHalfOpenTradeBuckets) {
    const auto result = build(request(), tape()); ASSERT_TRUE(result.error.empty()) << result.error;
    SentinelStreamClient client("127.0.0.1", "0");
    std::vector<FootprintSlice> foot;
    std::vector<TpoSlice> tpo;
    std::vector<VolumeProfileSlice> vp;
    QObject::connect(&client, &SentinelStreamClient::footprintSliceReceived, &client, [&](auto s) { foot.push_back(s); });
    QObject::connect(&client, &SentinelStreamClient::tpoSliceReceived, &client, [&](auto s) { tpo.push_back(s); });
    QObject::connect(&client, &SentinelStreamClient::volumeProfileSliceReceived, &client, [&](auto s) { vp.push_back(s); });
    for (const auto& message : result.messages) TradeOverlayWireTest::receive(client, nlohmann::json::parse(message));
    ASSERT_EQ(foot.size(), 2); ASSERT_EQ(tpo.size(), 1); ASSERT_EQ(vp.size(), 1);
    EXPECT_EQ(foot[0].bucketStartMs, day + 60000); EXPECT_EQ(foot[0].bucketEndMs, day + 120000);
    EXPECT_EQ(foot[1].bucketStartMs, day + 120000);
    for (const auto& s : foot) {
        EXPECT_EQ(s.gridWidth, 16); EXPECT_EQ(s.gridHeight, 10);
        EXPECT_DOUBLE_EQ(s.tickSize, 2); EXPECT_DOUBLE_EQ(s.minPrice, 100); EXPECT_DOUBLE_EQ(s.maxPrice, 120);
    }
    const auto cell = [](const FootprintSlice& s, int row) {
        return (int(qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(s.deltaLevelsQ16.constData()) + row * 2)) - 32768) * s.quantScale;
    };
    EXPECT_NEAR(cell(foot[0], 2), 2, .0001); EXPECT_NEAR(cell(foot[0], 4), 0, .0001);
    EXPECT_NEAR(cell(foot[1], 4), -5, .0001);
    EXPECT_EQ(tpo[0].letters[2], 'A'); EXPECT_EQ(tpo[0].letters[4], 'A');
    EXPECT_DOUBLE_EQ(tpo[0].tickSize, 2); EXPECT_DOUBLE_EQ(vp[0].tickSize, 2);
    EXPECT_DOUBLE_EQ(vp[0].totalVolume, 9);
}
TEST_F(TradeOverlay, HistoryAndLiveShareGridAndNoHeatmapMessages) {
    auto q = request(); q.kind = Kind::FootprintHistory;
    const auto result = build(q, tape()); ASSERT_TRUE(result.error.empty());
    auto j = nlohmann::json::parse(result.messages.at(0));
    EXPECT_EQ(j.at("type"), "footprint_history_chunk"); ASSERT_EQ(j["columns"].size(), 2);
    EXPECT_EQ(j["columns"][1]["time_start"], day + 60000);
    EXPECT_EQ(j["columns"][1]["max_price"], q.grid.maxPrice);
    q.kind = Kind::TpoHistory; q.nowMs = day + 1800001;
    const auto tpo = build(q, tape()); ASSERT_TRUE(tpo.error.empty());
    j = nlohmann::json::parse(tpo.messages.at(0));
    EXPECT_EQ(j["columns"][0]["time_start"], day);
    EXPECT_EQ(j["columns"][1]["time_start"], day + 900000);
}
TEST_F(TradeOverlay, InvalidGridAndTradeBudgetNeverYieldPartialMessages) {
    auto q = request(); q.grid.tick = std::numeric_limits<double>::quiet_NaN();
    auto r = build(q, tape()); EXPECT_FALSE(r.error.empty()); EXPECT_TRUE(r.messages.empty());
    q = request(); q.grid.rows = kMaxRows + 1;
    EXPECT_FALSE(build(q, tape()).error.empty());
    q = request(); std::vector<ServerDataModel::FootprintTradeSample> tooMany(kMaxTrades + 1);
    EXPECT_FALSE(build(q, tooMany).error.empty());
    q.tpoMs = 70000; EXPECT_FALSE(build(q, tape()).error.empty());
}
TEST_F(TradeOverlay, MappingUsesOwnBandAndClipsOnCommonViewport) {
    TradeOverlayGrid grid{60000, 180000, 120, 2, 1};
    const auto m = mapTradeOverlay(grid, 2, 10, 0, 240000, 90, 130, {0,0,800,400});
    EXPECT_EQ(m.draw, QRectF(200,100,400,200)); EXPECT_EQ(m.source, QRectF(0,0,2,10));
    // Heatmap dimensions, tick, placement and bands are deliberately absent from this API.
    const auto zoom = mapTradeOverlay(grid, 2, 10, 90000,150000,105,115,{0,0,800,400});
    EXPECT_EQ(zoom.draw, QRectF(0,0,800,400)); EXPECT_EQ(zoom.source, QRectF(.5,2.5,1,5));
    const auto disjoint = mapTradeOverlay(grid, 2,10,0,30000,90,130,{0,0,800,400});
    EXPECT_TRUE(disjoint.draw.isEmpty());
}
TEST_F(TradeOverlay, FootprintGridResetAndSkippedBucketsDoNotStretchTime) {
    FootprintStreamState ring; QByteArray data(20,'\0');
    ASSERT_TRUE(ring.ingestSlice(60000,120000,60000,4,10,100,120,2,data));
    auto first = ring.snapshot();
    ASSERT_TRUE(ring.ingestSlice(180000,240000,60000,4,10,100,120,2,data));
    EXPECT_EQ(ring.snapshot().filledColumns,3);
    QByteArray gap; ASSERT_TRUE(ring.copyColumnForUpload(1,gap));
    EXPECT_EQ(qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(gap.constData())), 0x8000);
    ASSERT_TRUE(ring.ingestSlice(180000,240000,60000,4,10,110,130,2,data));
    EXPECT_GT(ring.snapshot().resetGeneration,first.resetGeneration);
    EXPECT_EQ(ring.snapshot().filledColumns,1);
    EXPECT_DOUBLE_EQ(ring.snapshot().maxPrice,130);
    ASSERT_TRUE(ring.ingestSlice(180000,360000,180000,4,10,110,130,2,data));
    EXPECT_EQ(ring.snapshot().timeframeMs,180000);
}
TEST_F(TradeOverlay, DataProcessorPublishesMetadataIndependentOfRecordingReband) {
    DataProcessor processor; processor.setActiveSymbol("BTC-USD");
    TradeOverlayGrid grid; int uploads = 0;
    QObject::connect(&processor, &DataProcessor::footprintColumnReady, &processor,
        [&](int,int,int,QByteArray,TradeOverlayGrid g) { grid = g; ++uploads; });
    FootprintSlice s; s.symbol="BTC-USD"; s.timeframeMs=60000;
    s.bucketStartMs=60000; s.bucketEndMs=120000; s.gridWidth=16; s.gridHeight=10;
    s.minPrice=100; s.maxPrice=120; s.tickSize=2; s.format="q16_delta"; s.deltaLevelsQ16=QByteArray(20,'\0');
    processor.onFootprintSliceReceived(s); ASSERT_EQ(uploads,1); const auto before=grid;
    processor.setRecordingConfig(true,2); processor.setHeatmapViewport(0,600000,false,1000,2000,800,400);
    processor.onFootprintSliceReceived(s); ASSERT_EQ(uploads,2);
    EXPECT_EQ(grid.startMs,before.startMs); EXPECT_EQ(grid.endMs,before.endMs);
    EXPECT_EQ(grid.maxPrice,before.maxPrice); EXPECT_EQ(grid.tick,before.tick);
    EXPECT_EQ(grid.generation,before.generation);
    s.symbol="ETH-USD"; processor.onFootprintSliceReceived(s); EXPECT_EQ(uploads,2);
}

TEST_F(TradeOverlay, AdvertisedDefaultsAreIndependentOfHeatmapConfig) {
    const auto config = protocol::clientparse::parseServerConfig({
        {"heatmap", {{"grid_width", 8192}, {"grid_height", 999}, {"tick_size", 100}}},
        {"trade_overlays", {{"grid_width", 256}, {"grid_height", 100}, {"tick_size", 2.5},
                            {"footprint_timeframe_ms", 300000}}}});
    EXPECT_EQ(config.tradeOverlays.gridWidth,256);
    EXPECT_EQ(config.tradeOverlays.gridHeight,100);
    EXPECT_DOUBLE_EQ(config.tradeOverlays.tickSize,2.5);
    EXPECT_EQ(config.tradeOverlays.footprintTimeframeMs,300000);
}

TEST_F(TradeOverlay, SnapshotBudgetsOnlyRequestedWindowAndSortsOnlyMatches) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    struct RestoreDirectory {
        QString path = QDir::currentPath();
        ~RestoreDirectory() { QDir::setCurrent(path); }
    } restore;
    ASSERT_TRUE(QDir::setCurrent(directory.path()));
    ServerConfig config; config.heatmap.persistenceEnabled = false; config.recording.enabled = false;
    ServerDataModel model(config);
    using Trade = ServerDataModel::FootprintTradeSample;
    std::deque<Trade> retained(kMaxTrades + 1, Trade{day, 100, 1, AggressorSide::Buy});
    // Deliberately out of arrival order; both endpoints of the query are tested.
    retained.push_back({day + 119999, 111, 2, AggressorSide::Sell});
    retained.push_back({day + 120000, 119, 9, AggressorSide::Buy});
    retained.push_back({day + 60000, 115, 3, AggressorSide::Buy});
    TradeOverlayModelTest::seed(model, "BTC-USD", std::move(retained));
    std::vector<Trade> trades; int64_t retainedFrom = 0;
    ASSERT_TRUE(model.collectOverlayTrades("BTC-USD", day + 60000, day + 120000,
                                           kMaxTrades, trades, &retainedFrom));
    EXPECT_EQ(retainedFrom, day);
    ASSERT_EQ(trades.size(), 2);
    EXPECT_EQ(trades[0].timestampMs, day + 60000); EXPECT_DOUBLE_EQ(trades[0].price, 115);
    EXPECT_EQ(trades[1].timestampMs, day + 119999); EXPECT_DOUBLE_EQ(trades[1].size, 2);
    EXPECT_TRUE(model.collectOverlayTrades("BTC-USD", day + 60000, day + 120000, 2, trades));
    EXPECT_FALSE(model.collectOverlayTrades("BTC-USD", day + 60000, day + 120000, 1, trades));
    EXPECT_TRUE(trades.empty());
    EXPECT_FALSE(model.collectOverlayTrades("BTC-USD", day, day + 120000, kMaxTrades, trades));
    EXPECT_TRUE(trades.empty());
    auto q = request(); q.kind = Kind::FootprintHistory;
    EXPECT_EQ(tradeWindow(q).startMs, day); EXPECT_EQ(tradeWindow(q).endMs, day + 120000);
}

TEST_F(TradeOverlay, ClosedSessionKeepsFirstHistoryPeriodAndSuppressesExtraLivePeriod) {
    constexpr int64_t hour = 3600000;
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::NY;
    q.tpoMs = hour; q.nowMs = day + 23 * hour; q.count = 9;
    const auto history = build(q, {{day + 13 * hour, 115, 1, AggressorSide::Buy}});
    ASSERT_TRUE(history.error.empty()) << history.error;
    auto j = nlohmann::json::parse(history.messages.at(0));
    ASSERT_EQ(j["columns"].size(), 9);
    EXPECT_EQ(j["columns"][0]["time_start"], day + 13 * hour);
    EXPECT_EQ(j["columns"][8]["time_end"], day + 22 * hour);
    const auto letters = QByteArray::fromBase64(QByteArray::fromStdString(j["columns"][0]["letters"].get<std::string>()));
    EXPECT_EQ(letters[2], 'A');
    EXPECT_EQ(tradeWindow(q).endMs, day + 22 * hour);
    q.kind = Kind::Live; q.previousMs = day + 22 * hour - 1;
    for (const auto now : {day + 22 * hour, day + 23 * hour}) {
        q.nowMs = now;
        const auto live = build(q, {}); ASSERT_TRUE(live.error.empty());
        int tpoCount = 0;
        for (const auto& message : live.messages) {
            const auto col = nlohmann::json::parse(message);
            if (col["type"] != "tpo_slice") continue;
            ++tpoCount;
            EXPECT_EQ(col["time_start"], day + 21 * hour);
            EXPECT_EQ(col["time_end"], day + 22 * hour);
        }
        EXPECT_EQ(tpoCount, now == day + 22 * hour ? 1 : 0);
    }
    TpoStreamState state; state.setSessionType(static_cast<int>(q.session));
    ASSERT_TRUE(state.ingestSlice(day + 21 * hour, day + 22 * hour, hour, 9, 10, QByteArray(10, 'I')));
    const auto before = state.snapshot();
    EXPECT_FALSE(state.ingestSlice(day + 22 * hour, day + 23 * hour, hour, 9, 10, QByteArray(10, 'J')));
    EXPECT_EQ(state.snapshot().lastSliceStartMs, before.lastSliceStartMs);
    EXPECT_EQ(state.snapshot().pendingUploads, before.pendingUploads);
}

TEST_F(TradeOverlay, CandleHistoryFillsOlderPeriodsAndPartialRetentionMinuteOnOwnGrid) {
    constexpr int64_t hour = 3600000;
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::NY;
    q.tpoMs = hour; q.nowMs = day + 23 * hour; q.count = 9;
    const auto retainedFrom = day + 20 * hour + 30000;
    std::vector<std::pair<int64_t, int64_t>> pages;
    const auto candles = fetchTpoCandles(q, retainedFrom, [&](int64_t startSec, int64_t endSec, int limit) {
        pages.emplace_back(startSec * 1000, endSec * 1000);
        EXPECT_LE(limit, 350); EXPECT_EQ(endSec - startSec, limit * 60);
        CandleFetchResult r; r.ok = true;
        for (auto t = startSec * 1000; t < endSec * 1000; t += 60000) {
            OHLCVBar bar; bar.timestamp_ms = t; bar.high = 117; bar.low = 113; bar.close = 115;
            r.candles.push_back(bar);
        }
        return r;
    });
    ASSERT_TRUE(candles.ok); ASSERT_EQ(pages.size(), 2);
    EXPECT_EQ(pages.front().first, day + 13 * hour);
    EXPECT_EQ(pages.front().second, pages.back().first);
    EXPECT_EQ(pages.back().second, day + 20 * hour + 60000);
    const auto history = build(q, {{retainedFrom, 109, 1, AggressorSide::Buy},
                                  {day + 21 * hour, 111, 2, AggressorSide::Sell}}, candles.candles, retainedFrom);
    ASSERT_TRUE(history.error.empty()) << history.error;
    SentinelStreamClient client("127.0.0.1", "0");
    std::vector<TpoSlice> columns;
    QObject::connect(&client, &SentinelStreamClient::tpoSliceReceived, &client, [&](auto s) { columns.push_back(s); });
    TradeOverlayWireTest::receive(client, nlohmann::json::parse(history.messages.at(0)));
    ASSERT_EQ(columns.size(), 9);
    EXPECT_EQ(columns[0].letters[1], 'A'); EXPECT_EQ(columns[0].letters[3], 'A');
    EXPECT_EQ(columns[6].letters[2], 'G');
    EXPECT_EQ(columns[7].letters[1], 'H'); EXPECT_EQ(columns[7].letters[5], 'H');
    EXPECT_EQ(columns[8].letters[1], '\0'); EXPECT_EQ(columns[8].letters[4], 'I');
    EXPECT_DOUBLE_EQ(columns[0].maxPrice, 120); EXPECT_DOUBLE_EQ(columns[0].tickSize, 2);
    // Tape covering the whole requested window needs no REST work.
    EXPECT_TRUE(fetchTpoCandles(q, day + 13 * hour, [](auto, auto, auto) {
        ADD_FAILURE() << "unnecessary candle fetch"; return CandleFetchResult{};
    }).candles.empty());
}

TEST_F(TradeOverlay, RestartWithoutTradesBackfillsClosedSessionAndAnchorsGridFromCandles) {
    constexpr int64_t hour = 3600000;
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::NY;
    q.tpoMs = hour; q.nowMs = day + 23 * hour; q.count = 9; q.grid.maxPrice = 0;
    int64_t lastEnd = 0;
    const auto candles = fetchTpoCandles(q, 0, [&](int64_t startSec, int64_t endSec, int) {
        lastEnd = endSec * 1000;
        CandleFetchResult r; r.ok = true;
        for (auto t = startSec * 1000; t < endSec * 1000; t += 60000) {
            OHLCVBar b; b.timestamp_ms = t; b.high = 117; b.low = 113; b.close = 115;
            r.candles.push_back(b);
        }
        return r;
    });
    ASSERT_TRUE(candles.ok); EXPECT_EQ(lastEnd, day + 22 * hour);
    const auto history = build(q, {}, candles.candles);
    ASSERT_TRUE(history.error.empty()) << history.error; EXPECT_TRUE(history.grid.valid());
    const auto j = nlohmann::json::parse(history.messages.at(0));
    ASSERT_EQ(j["columns"].size(), 9);
    for (int i = 0; i < 9; ++i) {
        const auto letters = QByteArray::fromBase64(QByteArray::fromStdString(j["columns"][i]["letters"].get<std::string>()));
        EXPECT_TRUE(letters.contains(char('A' + i)));
    }
    const auto failed = fetchTpoCandles(q, 0, [](auto, auto, auto) {
        CandleFetchResult r; r.error = "REST unavailable"; return r;
    });
    EXPECT_FALSE(failed.ok); EXPECT_EQ(failed.error, "REST unavailable"); EXPECT_TRUE(failed.candles.empty());
}

TEST_F(TradeOverlay, CancelledCandleHistoryDoesNotFetchAnotherPageOrReturnPartialHistory) {
    auto q = request(); q.kind = Kind::TpoHistory; q.nowMs = day + 86400000; q.count = 96;
    bool stopped = false; int calls = 0;
    const auto fetch = [&](auto, auto, auto) {
        ++calls; stopped = true;
        CandleFetchResult page; page.ok = true; page.candles.push_back({}); return page;
    };
    auto result = fetchTpoCandles(q, 0, fetch, [&] { return stopped; });
    EXPECT_EQ(calls, 1); EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    result = fetchTpoCandles(q, 0, fetch, [&] { return stopped; });
    EXPECT_EQ(calls, 1); EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
}

TEST_F(TradeOverlay, WeeklyAndMonthlyTpoUseCoarserCentredGridFootprintKeepsBase) {
    const Grid base{16, 10, 2, 120};  // [100, 120)
    EXPECT_DOUBLE_EQ(tpoGridFor(base, SessionManager::SessionType::H24).tick, 2);
    const auto week = tpoGridFor(base, SessionManager::SessionType::W1);
    EXPECT_EQ(week.rows, 10);
    EXPECT_DOUBLE_EQ(week.tick, 10);
    EXPECT_DOUBLE_EQ(week.maxPrice, 160);  // centre 110, +/- 50, aligned to 10
    EXPECT_DOUBLE_EQ(week.minPrice(), 60);
    const auto month = tpoGridFor(base, SessionManager::SessionType::M1);
    EXPECT_DOUBLE_EQ(month.tick, 20);
    EXPECT_GE(month.minPrice(), 0);  // never below zero
    EXPECT_TRUE(month.valid());

    // Monday 2026-09-28 00:00 UTC lies inside the W1 session opened Sunday 21:00.
    constexpr int64_t monday = 1790553600000;
    auto q = request(); q.kind = Kind::Live; q.session = SessionManager::SessionType::W1;
    q.tpoMs = 3600000; q.nowMs = monday + 120001; q.previousMs = monday + 119001;
    const std::vector<ServerDataModel::FootprintTradeSample> trades{
        {monday + 60000, 115, 3, AggressorSide::Buy}, {monday + 119999, 111, 1, AggressorSide::Sell}};
    const auto result = build(q, trades);
    ASSERT_TRUE(result.error.empty()) << result.error;
    bool sawTpo = false, sawFootprint = false;
    for (const auto& message : result.messages) {
        const auto j = nlohmann::json::parse(message);
        if (j["type"] == "tpo_slice") {
            sawTpo = true;
            EXPECT_DOUBLE_EQ(j["tick_size"].get<double>(), 10);
            EXPECT_DOUBLE_EQ(j["max_price"].get<double>(), 160);
            const auto letters = QByteArray::fromBase64(QByteArray::fromStdString(j["letters"].get<std::string>()));
            // 115 and 111 fall in row floor((160 - p) / 10) = 4.
            EXPECT_NE(letters[4], '\0');
            EXPECT_EQ(letters[3], '\0');
        } else if (j["type"] == "footprint_slice") {
            sawFootprint = true;
            EXPECT_DOUBLE_EQ(j["tick_size"].get<double>(), 2);
        }
    }
    EXPECT_TRUE(sawTpo);
    EXPECT_TRUE(sawFootprint);
}
