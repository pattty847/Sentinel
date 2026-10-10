#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QtEndian>
#include "protocol/SentinelStreamClientParseHelpers.hpp"
#include "servermodel/TradeOverlayPublisher.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "render/TradeOverlayMapping.hpp"
#include "render/VolumeProfileRenderer.hpp"

#include <QSGGeometryNode>
#include <cmath>
#include <utility>
#include "render/FootprintStreamState.hpp"
#include "render/TpoStreamState.hpp"
#include "render/DataProcessor.hpp"

struct TradeOverlayWireTest {
    static void receive(SentinelStreamClient& client, const nlohmann::json& msg) {
        const auto type = msg.at("type");
        if (type == "trade") client.handleTradeMessage(msg);
        else if (type == "footprint_slice") client.handleFootprintSliceMessage(msg);
        else if (type == "footprint_history_chunk") client.handleFootprintHistoryChunkMessage(msg);
        else if (type == "tpo_slice") client.handleTpoSliceMessage(msg);
        else if (type == "tpo_history_chunk") client.handleTpoHistoryChunkMessage(msg);
        else if (type == "volume_profile_slice") client.handleVolumeProfileSliceMessage(msg);
    }
    static void expectTpo(SentinelStreamClient& client, const std::string& id) { client.m_expectedTpoRequestId = id; }
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
// The footprint grid is the overlay's own (INV-068): a repeated slice keeps its
// metadata; a chart timeframe change on the processor does not reset it either.
TEST_F(TradeOverlay, DataProcessorPublishesStableFootprintGridMetadata) {
    DataProcessor processor; processor.setActiveSymbol("BTC-USD");
    TradeOverlayGrid grid; int uploads = 0;
    QObject::connect(&processor, &DataProcessor::footprintColumnReady, &processor,
        [&](int,int,int,QByteArray,TradeOverlayGrid g) { grid = g; ++uploads; });
    FootprintSlice s; s.symbol="BTC-USD"; s.timeframeMs=60000;
    s.bucketStartMs=60000; s.bucketEndMs=120000; s.gridWidth=16; s.gridHeight=10;
    s.minPrice=100; s.maxPrice=120; s.tickSize=2; s.format="q16_delta"; s.deltaLevelsQ16=QByteArray(20,'\0');
    processor.onFootprintSliceReceived(s); ASSERT_EQ(uploads,1); const auto before=grid;
    processor.setTimeframe(300000);
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
    const auto candles = fetchTpoCandles(q, retainedFrom, [&](int64_t startSec, int64_t endSec, int64_t granSec, int limit) {
        pages.emplace_back(startSec * 1000, endSec * 1000);
        EXPECT_EQ(granSec, 3600);  // hourly periods from 13:00: ONE_HOUR candles
        EXPECT_LE(limit, 350); EXPECT_EQ(endSec - startSec, limit * granSec);
        CandleFetchResult r; r.ok = true;
        for (auto t = startSec * 1000; t < endSec * 1000; t += granSec * 1000) {
            OHLCVBar bar; bar.timestamp_ms = t; bar.high = 117; bar.low = 113; bar.close = 115;
            r.candles.push_back(bar);
        }
        return r;
    });
    // One call covers the session up to the candle holding the first retained trade.
    ASSERT_TRUE(candles.ok); ASSERT_EQ(pages.size(), 1);
    EXPECT_EQ(pages.front().first, day + 13 * hour);
    EXPECT_EQ(pages.front().second, day + 21 * hour);
    q.requestId = "tpo-1-0";
    const auto history = build(q, {{retainedFrom, 109, 1, AggressorSide::Buy},
                                  {day + 21 * hour, 111, 2, AggressorSide::Sell}}, candles.candles, retainedFrom);
    ASSERT_TRUE(history.error.empty()) << history.error;
    SentinelStreamClient client("127.0.0.1", "0");
    TradeOverlayWireTest::expectTpo(client, "tpo-1-0");
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
    EXPECT_TRUE(fetchTpoCandles(q, day + 13 * hour, [](auto, auto, auto, auto) {
        ADD_FAILURE() << "unnecessary candle fetch"; return CandleFetchResult{};
    }).candles.empty());
}

TEST_F(TradeOverlay, RestartWithoutTradesBackfillsClosedSessionAndAnchorsGridFromCandles) {
    constexpr int64_t hour = 3600000;
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::NY;
    q.tpoMs = hour; q.nowMs = day + 23 * hour; q.count = 9; q.grid.maxPrice = 0;
    int64_t lastEnd = 0;
    const auto candles = fetchTpoCandles(q, 0, [&](int64_t startSec, int64_t endSec, int64_t granSec, int) {
        lastEnd = endSec * 1000;
        CandleFetchResult r; r.ok = true;
        for (auto t = startSec * 1000; t < endSec * 1000; t += granSec * 1000) {
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
    const auto failed = fetchTpoCandles(q, 0, [](auto, auto, auto, auto) {
        CandleFetchResult r; r.error = "REST unavailable"; return r;
    });
    EXPECT_FALSE(failed.ok); EXPECT_EQ(failed.error, "REST unavailable"); EXPECT_TRUE(failed.candles.empty());
}

TEST_F(TradeOverlay, CancelledCandleHistoryDoesNotFetchAnotherPageOrReturnPartialHistory) {
    auto q = request(); q.kind = Kind::TpoHistory; q.nowMs = day + 86400000; q.count = 96;
    bool stopped = false; int calls = 0;
    const auto fetch = [&](auto, auto, auto, auto) {
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

    // Monday 2026-09-28 00:00 UTC is a W1 session open.
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

TEST_F(TradeOverlay, WeekendTradeLandsInWeeklyProfileAndVolumeProfileStaysOnTheUtcDay) {
    constexpr int64_t hour = 3600000;
    constexpr int64_t monday = 1790553600000;          // 2026-09-28 00:00 UTC, W1 open
    constexpr int64_t saturday = monday + 5 * 86400000; // 2026-10-03 00:00 UTC
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::W1;
    q.tpoMs = 4 * hour; q.nowMs = saturday + 13 * hour; q.count = 512;
    const auto history = build(q, {{saturday + 10 * hour, 115, 1, AggressorSide::Buy}});
    ASSERT_TRUE(history.error.empty()) << history.error;
    const auto j = nlohmann::json::parse(history.messages.at(0));
    EXPECT_EQ(j["grid_width"], 42);  // 7 days of 4 h periods
    bool found = false;
    for (const auto& column : j["columns"]) {
        if (column["time_start"] != saturday + 8 * hour) continue;
        const auto letters = QByteArray::fromBase64(QByteArray::fromStdString(column["letters"].get<std::string>()));
        for (char c : letters) found = found || c != '\0';
        // Saturday 08:00 is period 32 of the week; the server's letter cycles A..Z.
        for (char c : letters) if (c != '\0') EXPECT_EQ(c, 'A' + 32 % 26);
    }
    EXPECT_TRUE(found) << "Saturday trade missing from the weekly profile";

    // The client letters by period index (A-Z, a-z); weekend days are inside W1:
    const auto w = SessionManager::sessionContaining(saturday + 10 * hour, SessionManager::SessionType::W1);
    EXPECT_EQ(w.startMs, monday);
    EXPECT_EQ(w.endMs, monday + 7 * 86400000);

    // A monthly TPO selection never widens the live volume profile beyond its UTC day.
    auto live = request(); live.kind = Kind::Live; live.session = SessionManager::SessionType::M1;
    live.tpoMs = 30 * 60000; live.nowMs = saturday + 13 * hour; live.previousMs = live.nowMs - 1000;
    EXPECT_EQ(tradeWindow(live).startMs, saturday);
    const auto result = build(live, {{saturday + 12 * hour, 115, 2, AggressorSide::Buy}});
    ASSERT_TRUE(result.error.empty()) << result.error;
    bool sawVp = false;
    for (const auto& message : result.messages) {
        const auto m = nlohmann::json::parse(message);
        if (m["type"] != "volume_profile_slice") continue;
        sawVp = true;
        EXPECT_EQ(m["session_type"], static_cast<int>(SessionManager::SessionType::H24));
        EXPECT_EQ(m["session_start_ms"], saturday);
        EXPECT_EQ(m["session_end_ms"], saturday + 86400000);
    }
    EXPECT_TRUE(sawVp);
}

TEST_F(TradeOverlay, TpoHistoryChunkEchoesRequestId) {
    auto q = request(); q.kind = Kind::TpoHistory; q.nowMs = day + 1800001; q.requestId = "tpo-7-2";
    const auto tpo = build(q, tape()); ASSERT_TRUE(tpo.error.empty());
    EXPECT_EQ(nlohmann::json::parse(tpo.messages.at(0))["request_id"], "tpo-7-2");
    q.requestId.clear();
    EXPECT_FALSE(nlohmann::json::parse(build(q, tape()).messages.at(0)).contains("request_id"));
}

TEST_F(TradeOverlay, CandleFallbackUsesCoarsestAlignedGranularityAndOneCallPerWeek) {
    constexpr int64_t minute = 60000, hour = 3600000;
    EXPECT_EQ(tpoCandleGranularityMs(30 * minute, 0), 30 * minute);
    EXPECT_EQ(tpoCandleGranularityMs(15 * minute, 0), 15 * minute);
    EXPECT_EQ(tpoCandleGranularityMs(4 * hour, 0), 2 * hour);       // 6h does not divide 4h
    EXPECT_EQ(tpoCandleGranularityMs(24 * hour, 0), 24 * hour);
    EXPECT_EQ(tpoCandleGranularityMs(2 * hour, 13 * hour), hour);   // NY opens at 13:00
    EXPECT_EQ(tpoCandleGranularityMs(24 * hour, 22 * hour), 2 * hour);  // Australia 22:00
    EXPECT_EQ(tpoCandleGranularityMs(7 * minute, 0), minute);
    EXPECT_STREQ(candleGranularityName(1800), "THIRTY_MINUTE");
    EXPECT_EQ(candleGranularityName(123), nullptr);

    // A whole 7-day W1 page at 30-minute periods is one THIRTY_MINUTE call.
    constexpr int64_t monday = 1790553600000;
    auto q = request(); q.kind = Kind::TpoHistory; q.session = SessionManager::SessionType::W1;
    q.tpoMs = 30 * minute; q.nowMs = monday + 7 * 86400000 + hour; q.endMs = monday + 7 * 86400000; q.count = 336;
    int calls = 0;
    const auto candles = fetchTpoCandles(q, 0, [&](int64_t startSec, int64_t endSec, int64_t granSec, int limit) {
        ++calls;
        EXPECT_EQ(granSec, 1800); EXPECT_EQ(limit, 336);
        EXPECT_EQ(startSec * 1000, monday); EXPECT_EQ(endSec * 1000, monday + 7 * 86400000);
        CandleFetchResult r; r.ok = true;
        for (auto t = startSec * 1000; t < endSec * 1000; t += granSec * 1000) {
            OHLCVBar b; b.timestamp_ms = t; b.high = 117; b.low = 113; b.close = 115;
            r.candles.push_back(b);
        }
        return r;
    });
    ASSERT_TRUE(candles.ok); EXPECT_EQ(calls, 1); EXPECT_EQ(candles.candles.size(), 336u);
    // Each period's letters are exactly its own candle's high/low range.
    const auto history = build(q, {}, candles.candles);
    ASSERT_TRUE(history.error.empty()) << history.error;
    const auto j = nlohmann::json::parse(history.messages.at(0));
    ASSERT_EQ(j["columns"].size(), 336);
    const auto letters = QByteArray::fromBase64(QByteArray::fromStdString(j["columns"][5]["letters"].get<std::string>()));
    const double tick = j["columns"][5]["tick_size"].get<double>();
    const double top = j["columns"][5]["max_price"].get<double>();
    for (int row = 0; row < letters.size(); ++row) {
        const double high = top - row * tick, low = high - tick;
        const bool inside = high > 113 && low <= 117;
        EXPECT_EQ(letters[row] != '\0', inside) << "row " << row;
    }
}

TEST_F(TradeOverlay, VolumeProfileFollowsIntradayTpoSessionsAndFallsBackToUtcDay) {
    using SessionManager::SessionType;
    EXPECT_EQ(volumeProfileSession(SessionType::NY), SessionType::NY);
    EXPECT_EQ(volumeProfileSession(SessionType::London), SessionType::London);
    EXPECT_EQ(volumeProfileSession(SessionType::Australia), SessionType::Australia);
    EXPECT_EQ(volumeProfileSession(SessionType::H24), SessionType::H24);
    EXPECT_EQ(volumeProfileSession(SessionType::W1), SessionType::H24);
    EXPECT_EQ(volumeProfileSession(SessionType::M1), SessionType::H24);

    constexpr int64_t hour = 3600000;
    auto q = request(); q.kind = Kind::Live; q.session = SessionType::NY; q.tpoMs = hour;
    q.nowMs = day + 15 * hour; q.previousMs = q.nowMs - 1000;
    EXPECT_EQ(tradeWindow(q).startMs, day + 13 * hour);
    const auto result = build(q, {{day + 14 * hour, 115, 2, AggressorSide::Buy}});
    ASSERT_TRUE(result.error.empty()) << result.error;
    bool sawVp = false;
    for (const auto& message : result.messages) {
        const auto m = nlohmann::json::parse(message);
        if (m["type"] != "volume_profile_slice") continue;
        sawVp = true;
        EXPECT_EQ(m["session_type"], static_cast<int>(SessionType::NY));
        EXPECT_EQ(m["session_start_ms"], day + 13 * hour);
        EXPECT_EQ(m["session_end_ms"], day + 22 * hour);
    }
    EXPECT_TRUE(sawVp);
}

TEST_F(TradeOverlay, TpoHistoryChunkWithoutTheInFlightRequestIdIsDroppedBeforeAnySlice) {
    auto q = request(); q.kind = Kind::TpoHistory; q.nowMs = day + 1800001; q.requestId = "tpo-3-0";
    const auto history = build(q, tape()); ASSERT_TRUE(history.error.empty());
    auto chunk = nlohmann::json::parse(history.messages.at(0));
    SentinelStreamClient client("127.0.0.1", "0");
    int slices = 0; QStringList replies;
    QObject::connect(&client, &SentinelStreamClient::tpoSliceReceived, &client, [&](auto) { ++slices; });
    QObject::connect(&client, &SentinelStreamClient::tpoHistoryChunkReceived, &client,
                     [&](const QString&, const QString& id, qint64, int, qint64, int) { replies << id; });
    TradeOverlayWireTest::expectTpo(client, "tpo-3-1");
    TradeOverlayWireTest::receive(client, chunk);           // stale id
    chunk.erase("request_id");
    TradeOverlayWireTest::receive(client, chunk);           // untagged
    EXPECT_EQ(slices, 0); EXPECT_TRUE(replies.isEmpty());
    chunk["request_id"] = "tpo-3-1";
    TradeOverlayWireTest::receive(client, chunk);
    EXPECT_EQ(slices, 2); EXPECT_EQ(replies, QStringList{"tpo-3-1"});
    TradeOverlayWireTest::receive(client, chunk);           // a duplicate is no longer expected
    EXPECT_EQ(slices, 2);
}

TEST_F(TradeOverlay, LiveTradeWirePreservesExchangeTimeAndNormalizesMakerSide) {
    SentinelStreamClient client("127.0.0.1", "0");
    Trade received{};
    int count = 0;
    QObject::connect(&client, &SentinelStreamClient::tradeReceived, &client,
                     [&](const Trade& t) { received = t; ++count; });
    nlohmann::json message{{"type","trade"},{"product_id","BTC-USD"},{"price",100.0},
        {"size",2.0},{"side","sell"},{"time","2026-10-03T12:34:56.789123Z"},{"trade_id","123"}};
    TradeOverlayWireTest::receive(client,message);
    ASSERT_EQ(count,1);
    EXPECT_EQ(received.product_id,"BTC-USD");
    EXPECT_EQ(received.trade_id,"123");
    EXPECT_EQ(received.price,100); EXPECT_EQ(received.size,2);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(received.timestamp.time_since_epoch()).count(),1791030896789LL);
    EXPECT_EQ(received.side,AggressorSide::Buy);
    message["side"]="buy";
    TradeOverlayWireTest::receive(client,message);
    EXPECT_EQ(received.side,AggressorSide::Sell);
    message["side_basis"]="aggressor";
    TradeOverlayWireTest::receive(client,message);
    EXPECT_EQ(received.side,AggressorSide::Buy);
    message["side"]="unknown"; message["time"]="bad";
    TradeOverlayWireTest::receive(client,message);
    EXPECT_EQ(received.side,AggressorSide::Unknown);
    EXPECT_EQ(received.timestamp.time_since_epoch().count(),0);
}

TEST_F(TradeOverlay, VolumeProfileDrawsOnlyFilledBarsAndReusesStorage) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 1;
    snap.gridHeight = 8;
    const std::vector<float> bins{1, 2, 3, 4, 5, 6, 7, 8};
    const QRectF surface(20, 30, 800, 400);
    renderer.render(&root, true, surface, 100, 108, bins, snap, 1.0, true);
    auto* bars = static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling());
    auto* geometry = bars->geometry();
    const auto* storage = geometry->vertexData();
    const auto* indices = geometry->indexData();

    // Exercise first allocation, fewer visible bins, disjoint prices, layer
    // off/on, and a new (smaller) profile grid. Unused triangles must never
    // connect zeroed spare vertices to a live bar.
    const auto check = [&](int filledBars) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        ASSERT_EQ(geometry->vertexCount(), filledBars * 4);
        ASSERT_EQ(geometry->indexCount(), filledBars * 6);
#endif
        const auto* vertices = geometry->vertexDataAsColoredPoint2D();
        const auto* drawnIndices = geometry->indexDataAsUShort();
        for (int i = 0; i < geometry->indexCount(); i += 3) {
            const auto a = drawnIndices[i], b = drawnIndices[i+1], c = drawnIndices[i+2];
            ASSERT_LT(a, geometry->vertexCount());
            ASSERT_LT(b, geometry->vertexCount());
            ASSERT_LT(c, geometry->vertexCount());
            if (i >= filledBars * 6) {
                EXPECT_EQ(a, b);
                EXPECT_EQ(b, c); // Qt < 6.10 retains capacity as degenerate triangles.
                continue;
            }
            for (auto index : {a, b, c}) {
                EXPECT_GE(vertices[index].x, surface.left());
                EXPECT_LE(vertices[index].x, surface.right());
                EXPECT_TRUE(std::isfinite(vertices[index].y));
            }
        }
        EXPECT_EQ(geometry->vertexData(), storage);
        EXPECT_EQ(geometry->indexData(), indices);
        EXPECT_EQ(root.childCount(), 3);
    };
    check(8);
    renderer.render(&root, true, surface, 99, 103.5, bins, snap, 1.0, true);
    check(4);
    renderer.render(&root, true, surface, 110, 118, bins, snap, 1.0, true);
    check(0);
    renderer.render(&root, false, surface, 100, 108, bins, snap, 1.0, true);
    check(0);
    snap.gridHeight = 2;
    const std::vector<float> smaller{3, 5};
    renderer.render(&root, true, surface, 100, 108, smaller, snap, 1.0, true);
    check(2);
}

TEST_F(TradeOverlay, VolumeProfileClearsInvalidValueAreaAndRetainsGrownBuffers) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 1;
    snap.gridHeight = 4097; // exceeds the initial allocation by one bin
    snap.va.valid = true;
    snap.va.valPrice = 100;
    snap.va.vahPrice = 110;
    snap.va.pocPrice = 105;
    const std::vector<float> bins(4097, 1);
    const QRectF surface(20, 30, 800, 400);
    renderer.render(&root, true, surface, 99, 4200, bins, snap, 1.0, true);
    auto* va = static_cast<QSGGeometryNode*>(root.firstChild());
    auto* bars = static_cast<QSGGeometryNode*>(va->nextSibling());
    auto* poc = static_cast<QSGGeometryNode*>(bars->nextSibling());
    const auto* storage = bars->geometry()->vertexData();
    const auto* indices = bars->geometry()->indexData();
    snap.va.valid = false;
    snap.gridHeight = 2;
    renderer.render(&root, true, surface, 99, 4200, {1, 2}, snap, 1.0, true);
    const auto hidden = [](QSGGeometry* geometry) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        EXPECT_EQ(geometry->vertexCount(), 0);
        EXPECT_EQ(geometry->indexCount(), 0);
#else
        const auto* data = geometry->indexDataAsUShort();
        for (int i = 0; i < geometry->indexCount(); ++i) EXPECT_EQ(data[i], 0);
#endif
    };
    hidden(va->geometry());
    hidden(poc->geometry());
    renderer.render(&root, false, surface, 99, 4200, bins, snap, 1.0, true);
    hidden(bars->geometry());
    snap.gridHeight = 4097;
    renderer.render(&root, true, surface, 99, 4200, bins, snap, 1.0, true);
    EXPECT_EQ(bars->geometry()->vertexData(), storage);
    EXPECT_EQ(bars->geometry()->indexData(), indices);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    EXPECT_EQ(bars->geometry()->vertexCount(), 4097 * 4);
    EXPECT_EQ(bars->geometry()->indexCount(), 4097 * 6);
#endif
}


TEST_F(TradeOverlay, VolumeProfileValueAreaPinsSeventyPercentRangeAndTranslucentGreen) {
    // Top-to-bottom rows: total 100, POC row 3 (40), then row 2 (20),
    // then row 4 (15). The smallest contiguous expansion reaching 70%
    // contains 75 and stops before either next neighbouring row (10).
    const std::vector<float> bins{1, 10, 20, 40, 15, 10, 3, 1};
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 5;
    snap.gridHeight = int(bins.size());
    snap.va = VolumeProfileState::computeValueArea(bins, snap.minPrice, snap.tickSize);
    ASSERT_TRUE(snap.va.valid);
    EXPECT_DOUBLE_EQ(snap.va.totalVolume, 100);
    EXPECT_EQ(snap.va.pocRow, 3);
    EXPECT_EQ(snap.va.vahRow, 2);
    EXPECT_EQ(snap.va.valRow, 4);
    EXPECT_DOUBLE_EQ(snap.va.pocPrice, 122.5);
    EXPECT_DOUBLE_EQ(snap.va.vahPrice, 130);
    EXPECT_DOUBLE_EQ(snap.va.valPrice, 115);

    VolumeProfileRenderer renderer;
    QSGNode root;
    const QRectF surface(20, 30, 800, 400);
    const auto check = [&](double priceMin, double priceMax, float top, float bottom) {
        renderer.render(&root, true, surface, priceMin, priceMax, bins, snap, 1.0, true);
        auto* va = static_cast<QSGGeometryNode*>(root.firstChild());
        auto* geometry = va->geometry();
        ASSERT_EQ(geometry->vertexCount(), 4);
        ASSERT_EQ(geometry->indexCount(), 6);
        const auto* v = geometry->vertexDataAsColoredPoint2D();
        for (int i = 0; i < 4; ++i) {
            EXPECT_FLOAT_EQ(v[i].x, i % 2 == 0 ? 724 : 820);
            EXPECT_FLOAT_EQ(v[i].y, i < 2 ? top : bottom);
            // QSGVertexColorMaterial consumes premultiplied RGBA. Alpha 60
            // is 24% opacity; original green (60,200,100) becomes (14,47,23).
            EXPECT_EQ(v[i].r, 14);
            EXPECT_EQ(v[i].g, 47);
            EXPECT_EQ(v[i].b, 23);
            EXPECT_EQ(v[i].a, 60);
        }
        auto* bars = static_cast<QSGGeometryNode*>(va->nextSibling());
        const auto* barVertices = bars->geometry()->vertexDataAsColoredPoint2D();
        const int visibleBars = priceMin == 100 ? 8 : 3;
        for (int i = 0; i < visibleBars * 4; ++i) {
            EXPECT_EQ(barVertices[i].r, 100 * int(barVertices[i].a) / 255);
            EXPECT_EQ(barVertices[i].g, 160 * int(barVertices[i].a) / 255);
            EXPECT_EQ(barVertices[i].b, 220 * int(barVertices[i].a) / 255);
        }
        auto* poc = static_cast<QSGGeometryNode*>(bars->nextSibling());
        const auto* pocVertices = poc->geometry()->vertexDataAsColoredPoint2D();
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(pocVertices[i].r, 240);
            EXPECT_EQ(pocVertices[i].g, 202);
            EXPECT_EQ(pocVertices[i].b, 0);
            EXPECT_EQ(pocVertices[i].a, 240);
        }
        const auto* idx = geometry->indexDataAsUShort();
        const quint16 expected[]{0, 1, 2, 1, 3, 2};
        for (int i = 0; i < 6; ++i) EXPECT_EQ(idx[i], expected[i]);
    };
    check(100, 140, 130, 280); // VA is fully visible.
    check(120, 125, 30, 430); // Both VA edges clip to the surface.
    renderer.setVaColor(QColor(40, 180, 240, 128));
    renderer.render(&root, true, surface, 100, 140, bins, snap, 1.0, true);
    const auto* v = static_cast<QSGGeometryNode*>(root.firstChild())->geometry()->vertexDataAsColoredPoint2D();
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(v[i].r, 20);
        EXPECT_EQ(v[i].g, 90);
        EXPECT_EQ(v[i].b, 120);
        EXPECT_EQ(v[i].a, 128);
    }
}


TEST_F(TradeOverlay, VolumeProfileClipsValueAreaAndPocAndHidesDisjointRects) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 5;
    snap.gridHeight = 8;
    const std::vector<float> bins{1, 10, 20, 40, 15, 10, 3, 1};
    snap.va = VolumeProfileState::computeValueArea(bins, 100, 5);
    const QRectF surface(20, 30, 800, 400);
    const auto checkRect = [&](QSGGeometryNode* node, float top, float bottom) {
        const auto* g = node->geometry();
        ASSERT_EQ(g->vertexCount(), 4);
        ASSERT_EQ(g->indexCount(), 6);
        const auto* v = g->vertexDataAsColoredPoint2D();
        for (int i = 0; i < 4; ++i) {
            EXPECT_FLOAT_EQ(v[i].y, i < 2 ? top : bottom);
            EXPECT_GE(v[i].x, surface.left());
            EXPECT_LE(v[i].x, surface.right());
        }
    };
    const auto hidden = [](QSGGeometryNode* node) {
        const auto* g = node->geometry();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        EXPECT_EQ(g->vertexCount(), 0);
        EXPECT_EQ(g->indexCount(), 0);
#else
        const auto* idx = g->indexDataAsUShort();
        for (int i = 0; i < g->indexCount(); ++i) EXPECT_EQ(idx[i], 0);
#endif
    };
    renderer.render(&root, true, surface, 100, 122.5, bins, snap, 1.0, true);
    auto* va = static_cast<QSGGeometryNode*>(root.firstChild());
    auto* poc = static_cast<QSGGeometryNode*>(va->nextSibling()->nextSibling());
    checkRect(va, 30, 163);
    checkRect(poc, 30, 31); // POC centred on the top edge.
    renderer.render(&root, true, surface, 122.5, 140, bins, snap, 1.0, true);
    checkRect(va, 258, 430);
    checkRect(poc, 428, 430); // POC centred on the bottom edge.
    for (const auto view : {std::pair{140.0, 160.0}, std::pair{80.0, 100.0}}) {
        renderer.render(&root, true, surface, view.first, view.second, bins, snap, 1.0, true);
        hidden(va);
        hidden(poc);
    }
    renderer.render(&root, true, surface, 100, 140, bins, snap, 1.0, true);
    checkRect(va, 130, 280); // Restores the retained geometry after hiding.
    checkRect(poc, 203, 206);
}

TEST_F(TradeOverlay, VolumeProfileCapsBarsAtUnsignedShortIndexLimit) {
    constexpr int maxBars = 16384;
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 1;
    snap.gridHeight = maxBars + 1;
    const std::vector<float> bins(maxBars + 1, 1);
    const QRectF surface(20, 30, 800, 400);
    renderer.render(&root, true, surface, 100, 100.0 + static_cast<double>(bins.size()), bins, snap, 1.0, true);
    auto* g = static_cast<QSGGeometryNode*>(root.firstChild()->nextSibling())->geometry();
    ASSERT_EQ(g->vertexCount(), maxBars * 4);
    ASSERT_EQ(g->indexCount(), maxBars * 6);
    const auto* idx = g->indexDataAsUShort();
    for (int bar = 0; bar < maxBars; ++bar) {
        const int first = bar * 4;
        const int expected[]{first, first+1, first+2, first+1, first+3, first+2};
        for (int i = 0; i < 6; ++i) EXPECT_EQ(int(idx[bar * 6 + i]), expected[i]);
    }
    EXPECT_EQ(idx[maxBars * 6 - 2], 65535);
}

TEST_F(TradeOverlay, VolumeProfileEdgesUseDevicePixelsOnlyAtRest) {
    VolumeProfileRenderer renderer;
    QSGNode root;
    VolumeProfileState::Snapshot snap;
    snap.minPrice = 100;
    snap.tickSize = 1;
    snap.gridHeight = 8;
    snap.va.valid = true;
    snap.va.vahPrice = 106;
    snap.va.valPrice = 102;
    snap.va.pocPrice = 104.5;
    const std::vector<float> bins(8, 1);
    const QRectF surface(0, 0, 800, 400);
    for (double dpr : {1.0, 2.0}) {
        for (bool free : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "dpr=" << dpr << " free=" << free);
            renderer.render(&root, true, surface, 99.3, 109.1, bins, snap, dpr, !free);
            const auto y = [&](double price) { return (109.1 - price) / 9.8 * 400; };
            const auto edge = [&](double value) { return free ? value : std::floor(value * dpr) / dpr; };
            auto* va = static_cast<QSGGeometryNode*>(root.firstChild());
            auto* bars = static_cast<QSGGeometryNode*>(va->nextSibling());
            auto* poc = static_cast<QSGGeometryNode*>(bars->nextSibling());
            const auto check = [&](QSGGeometryNode* node, int offset, double top, double bottom) {
                const auto* v = node->geometry()->vertexDataAsColoredPoint2D() + offset;
                EXPECT_FLOAT_EQ(v[0].y, edge(top));
                EXPECT_FLOAT_EQ(v[2].y, edge(bottom));
                if (!free) {
                    EXPECT_FLOAT_EQ(v[0].y * dpr, std::round(v[0].y * dpr));
                    EXPECT_FLOAT_EQ(v[2].y * dpr, std::round(v[2].y * dpr));
                }
            };
            check(va, 0, y(106), y(102));
            for (int i = 0; i < 8; ++i) check(bars, 4 * i, y(108 - i), y(107 - i));
            check(poc, 0, y(104.5) - 1.5, y(104.5) + 1.5);
        }
    }
}
