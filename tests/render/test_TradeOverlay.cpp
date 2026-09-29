#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QtEndian>
#include "protocol/SentinelStreamClientParseHelpers.hpp"
#include "servermodel/TradeOverlayPublisher.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "render/TradeOverlayMapping.hpp"
#include "render/FootprintStreamState.hpp"
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
