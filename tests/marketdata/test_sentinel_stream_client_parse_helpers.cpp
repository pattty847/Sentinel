#include <gtest/gtest.h>
#include "protocol/SentinelStreamClientParseHelpers.hpp"

TEST(SentinelStreamClientParseHelpers, ParseServerConfigMapsFields) {
    nlohmann::json msg = {
        {"timeframes_ms", nlohmann::json::array({0, 1000, -1, 60000})},
        {"heatmap",
         {{"grid_width", 4096},
          {"grid_height", 1024},
          {"tick_size", 0.5},
          {"active_timeframe_ms", 60000},
          {"served_timeframes_ms", nlohmann::json::array({0, 60000, -1, 300000})}}},
        {"orderbook", {{"tick_size", 0.01}, {"band_pct", 0.25}}},
        {"candles", {{"update_bps_fast", 0.1}, {"update_tick_size", 0.5}}},
        {"default_symbols", nlohmann::json::array({"BTC-USD", 123, "ETH-USD"})}
    };

    const ServerConfig cfg = protocol::clientparse::parseServerConfig(msg);
    ASSERT_EQ(cfg.heatmap.timeframesMs.size(), 2u);
    EXPECT_EQ(cfg.heatmap.timeframesMs[0], 1000);
    EXPECT_EQ(cfg.heatmap.timeframesMs[1], 60000);
    EXPECT_EQ(cfg.heatmap.gridWidth, 4096);
    EXPECT_EQ(cfg.heatmap.gridHeight, 1024);
    EXPECT_DOUBLE_EQ(cfg.heatmap.tickSize, 0.5);
    EXPECT_EQ(cfg.heatmap.activeTimeframeMs, 60000);
    EXPECT_EQ(cfg.heatmap.servedTimeframesMs, (std::vector<int64_t>{60000, 300000}));
    EXPECT_TRUE(cfg.wasAdvertised("heatmap.servedTimeframesMs"));
    EXPECT_TRUE(cfg.wasAdvertised("orderbook.tickSize"));
    EXPECT_FALSE(cfg.wasAdvertised("candles.bpsSlow"));
    EXPECT_DOUBLE_EQ(cfg.orderbook.tickSize, 0.01);
    EXPECT_DOUBLE_EQ(cfg.orderbook.bandPct, 0.25);
    EXPECT_DOUBLE_EQ(cfg.candles.bpsFast, 0.1);
    EXPECT_DOUBLE_EQ(cfg.candles.tickSize, 0.5);
    ASSERT_EQ(cfg.defaultSymbols.size(), 2u);
    EXPECT_EQ(cfg.defaultSymbols[0], "BTC-USD");
    EXPECT_EQ(cfg.defaultSymbols[1], "ETH-USD");
}

TEST(SentinelStreamClientParseHelpers, MissingServedTimeframesMeansUnknown) {
    const auto cfg = protocol::clientparse::parseServerConfig({
        {"timeframes_ms", nlohmann::json::array({1000, 60000})},
        {"heatmap", {{"active_timeframe_ms", 60000}}}
    });
    EXPECT_TRUE(cfg.heatmap.servedTimeframesMs.empty());
    EXPECT_FALSE(cfg.wasAdvertised("heatmap.servedTimeframesMs"));
}

namespace {
nlohmann::json serverVolumeProfileSlice() {
    return {
        {"type", "volume_profile_slice"},
        {"schema_version", protocol::SentinelProtocol::kVolumeProfileSchemaVersion},
        {"symbol", "BTC-USD"},
        {"session_start_ms", 1000},
        {"session_end_ms", 2000},
        {"session_type", 4},
        {"grid_height", 1},
        {"min_price", 100.0},
        {"max_price", 101.0},
        {"tick_size", 1.0},
        {"total_volume", 1.0},
        {"poc_price", 100.0},
        {"vah_price", 100.0},
        {"val_price", 100.0},
        {"format", "vp_f32"},
        {"encoding", "base64"},
        {"volume_bins", "AACAPw=="}
    };
}
} // namespace

TEST(SentinelStreamClientParseHelpers, VolumeProfileCanonicalSessionKeysAccepted) {
    auto msg = serverVolumeProfileSlice();
    const auto session = protocol::clientparse::parseVolumeProfileSessionBounds(msg);
    EXPECT_TRUE(session.valid());
    EXPECT_EQ(session.startMs, 1000);
    EXPECT_EQ(session.endMs, 2000);

    msg["session_start"] = 10;
    msg["session_end"] = 20;
    const auto preferred = protocol::clientparse::parseVolumeProfileSessionBounds(msg);
    EXPECT_EQ(preferred.startMs, 1000);
    EXPECT_EQ(preferred.endMs, 2000);
}

TEST(SentinelStreamClientParseHelpers, VolumeProfileLegacySessionKeysAccepted) {
    auto msg = serverVolumeProfileSlice();
    msg["session_start"] = msg["session_start_ms"];
    msg["session_end"] = msg["session_end_ms"];
    msg.erase("session_start_ms");
    msg.erase("session_end_ms");

    const auto session = protocol::clientparse::parseVolumeProfileSessionBounds(msg);
    EXPECT_TRUE(session.valid());
    EXPECT_EQ(session.startMs, 1000);
    EXPECT_EQ(session.endMs, 2000);
}

TEST(SentinelStreamClientParseHelpers, ParseCandleBarUsesDefaults) {
    nlohmann::json item = {
        {"time_start_ms", 10},
        {"time_end_ms", 20},
        {"open", 1.0},
        {"close", 2.0}
    };
    const auto bar = protocol::clientparse::parseCandleBar(item);
    EXPECT_EQ(bar.timeStartMs, 10);
    EXPECT_EQ(bar.timeEndMs, 20);
    EXPECT_DOUBLE_EQ(bar.open, 1.0);
    EXPECT_DOUBLE_EQ(bar.high, 0.0);
    EXPECT_DOUBLE_EQ(bar.low, 0.0);
    EXPECT_DOUBLE_EQ(bar.close, 2.0);
    EXPECT_DOUBLE_EQ(bar.volume, 0.0);
    EXPECT_FALSE(bar.isClosed);
}

TEST(SentinelStreamClientParseHelpers, ParseOrderBookLevelsRequiresPAndQ) {
    nlohmann::json levels = nlohmann::json::array({
        nlohmann::json{{"p", 100.0}, {"q", 2.0}},
        nlohmann::json{{"p", 101.0}},
        nlohmann::json{{"q", 3.0}},
        nlohmann::json{{"p", 102.0}, {"q", 4.0}}
    });
    const auto out = protocol::clientparse::parseOrderBookLevels(levels);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0].price, 100.0);
    EXPECT_DOUBLE_EQ(out[0].size, 2.0);
    EXPECT_DOUBLE_EQ(out[1].price, 102.0);
    EXPECT_DOUBLE_EQ(out[1].size, 4.0);
}

TEST(SentinelStreamClientParseHelpers, ParseL2UpdatesMapsSidePriceSize) {
    nlohmann::json deltas = nlohmann::json::array({
        nlohmann::json{{"side", "bid"}, {"price", 100.0}, {"size", 1.5}},
        nlohmann::json{{"side", "ask"}, {"price", 101.0}, {"size", 2.5}},
        nlohmann::json{{"price", 0.0}}
    });

    const auto out = protocol::clientparse::parseL2Updates(deltas);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_TRUE(out[0].isBid);
    EXPECT_FALSE(out[1].isBid);
    EXPECT_FALSE(out[2].isBid);
    EXPECT_DOUBLE_EQ(out[0].price, 100.0);
    EXPECT_DOUBLE_EQ(out[1].quantity, 2.5);
    EXPECT_DOUBLE_EQ(out[2].quantity, 0.0);
}
