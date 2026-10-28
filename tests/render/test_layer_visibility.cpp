#include "chart/shell/LayerVisibility.hpp"
#include <QVariant>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
using Events = std::vector<std::string>;
TEST(LayerVisibility, PrimaryFieldAndExclusionRules) {
    chart::LayerVisibility layers;
    Events events;
    layers.report = [&](std::string_view, bool, bool) { events.emplace_back("log"); };
    layers.requestUpdate = [&] { events.emplace_back("update"); };
    QObject::connect(&layers, &chart::LayerVisibility::primaryFieldChanged, [&] { events.emplace_back("primary"); });
    QObject::connect(&layers, &chart::LayerVisibility::layerVisibilityChanged, [&] { events.emplace_back("visibility"); });
    EXPECT_TRUE(layers.heatmapEnabled());
    layers.setPrimaryField(1);
    EXPECT_TRUE(layers.heatmapEnabled());
    EXPECT_TRUE(layers.footprintEnabled());
    EXPECT_EQ(events, (Events{"log", "update", "primary", "visibility"}));
    events.clear();
    layers.setPrimaryField(1);
    EXPECT_TRUE(events.empty());
    layers.setPrimaryField(2);
    EXPECT_TRUE(layers.tpoEnabled());
    EXPECT_FALSE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.footprintEnabled());
    layers.setPrimaryField(3);
    EXPECT_TRUE(layers.volumeProfileEnabled());
    EXPECT_FALSE(layers.tpoEnabled());
    layers.setPrimaryField(0);
    EXPECT_TRUE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.volumeProfileEnabled());
    layers.setPrimaryField(9);
    EXPECT_EQ(layers.primaryField(), 9);
    EXPECT_TRUE(layers.heatmapEnabled());
}
TEST(LayerVisibility, IndividualSettersKeepHeatmapAndFootprintCoexistence) {
    chart::LayerVisibility layers;
    Events events;
    layers.report = [&](std::string_view name, bool enabled, bool unchanged) {
        events.emplace_back(std::string(name) + (enabled ? "+" : "-") + (unchanged ? "=" : "!"));
    };
    layers.requestUpdate = [&] { events.emplace_back("update"); };
    QObject::connect(&layers, &chart::LayerVisibility::layerVisibilityChanged, [&] { events.emplace_back("visibility"); });
    layers.setFootprintEnabled(true);
    EXPECT_TRUE(layers.heatmapEnabled());
    EXPECT_TRUE(layers.footprintEnabled());
    EXPECT_EQ(events, (Events{"footprint+!", "update", "visibility"}));
    events.clear();
    layers.setFootprintEnabled(true);
    EXPECT_EQ(events, (Events{"footprint+="}));
    events.clear();
    layers.setTpoEnabled(true);
    EXPECT_TRUE(layers.tpoEnabled());
    EXPECT_FALSE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.footprintEnabled());
    layers.setHeatmapEnabled(true);
    EXPECT_TRUE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.tpoEnabled());
    layers.setFootprintEnabled(true);
    EXPECT_TRUE(layers.footprintEnabled());
    layers.setVolumeProfileEnabled(true);
    EXPECT_TRUE(layers.volumeProfileEnabled());
    EXPECT_FALSE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.footprintEnabled());
    layers.setTpoEnabled(true);
    EXPECT_TRUE(layers.tpoEnabled());
    EXPECT_FALSE(layers.volumeProfileEnabled());
    layers.setTpoEnabled(false);
    EXPECT_FALSE(layers.tpoEnabled());
    EXPECT_FALSE(layers.heatmapEnabled());
    layers.setVolumeProfileEnabled(false);
    layers.setHeatmapEnabled(false);
    layers.setFootprintEnabled(false);
    EXPECT_FALSE(layers.heatmapEnabled());
    EXPECT_FALSE(layers.footprintEnabled());
}
// The book ladder is independent of the primary field and off by default; its strip
// width is reserved only while it is on.
TEST(LayerVisibility, LadderIsIndependentAndReservesItsStripOnlyWhenOn) {
    chart::LayerVisibility layers;
    Events events;
    layers.report = [&](std::string_view name, bool enabled, bool) { events.emplace_back(std::string(name) + (enabled ? "+" : "-")); };
    EXPECT_FALSE(layers.ladderEnabled());
    EXPECT_EQ(layers.ladderWidthPx(), 0);
    EXPECT_EQ(layers.property("ladderWidthPx").toInt(), 0);
    layers.setLadderEnabled(true);
    EXPECT_TRUE(layers.ladderEnabled());
    EXPECT_EQ(layers.ladderWidthPx(), chart::LayerVisibility::kLadderWidthPx);
    EXPECT_EQ(layers.property("ladderLayerEnabled").toBool(), true);
    EXPECT_TRUE(layers.heatmapEnabled()) << "the primary layers are untouched";
    layers.setPrimaryField(2);
    EXPECT_TRUE(layers.ladderEnabled()) << "a primary field change keeps the ladder";
    layers.setLayerEnabled("ladder", false);
    EXPECT_EQ(layers.ladderWidthPx(), 0);
    EXPECT_EQ(events, (std::vector<std::string>{"ladder+", "primaryField-", "ladder-"}));
    // The strip width setting: clamped, applied only while the ladder is on.
    layers.setLadderWidthPx(200);
    EXPECT_EQ(layers.ladderWidthPx(), 0);
    layers.setLadderEnabled(true);
    EXPECT_EQ(layers.property("ladderWidthPx").toInt(), 200);
    layers.setLadderWidthPx(10);
    EXPECT_EQ(layers.ladderWidthPx(), chart::LayerVisibility::kLadderMinWidthPx);
    layers.setLadderWidthPx(1000);
    EXPECT_EQ(layers.ladderWidthPx(), chart::LayerVisibility::kLadderMaxWidthPx);
    EXPECT_EQ(chart::LayerVisibility::kLadderWidthPx, 120);
}

// The wall radar is independent of the primary field and the ladder flag; its threshold clamps.
TEST(LayerVisibility, WallRadarIsIndependentAndItsMultipleClamps) {
    chart::LayerVisibility layers;
    Events events;
    layers.report = [&](std::string_view name, bool enabled, bool) { events.emplace_back(std::string(name) + (enabled ? "+" : "-")); };
    int changes = 0;
    QObject::connect(&layers, &chart::LayerVisibility::layerVisibilityChanged, [&] { ++changes; });
    EXPECT_FALSE(layers.wallRadarEnabled());
    EXPECT_DOUBLE_EQ(layers.wallMultiple(), 4);
    layers.setWallRadarEnabled(true);
    EXPECT_TRUE(layers.property("wallRadarEnabled").toBool());
    EXPECT_FALSE(layers.ladderEnabled()) << "the layer flag does not turn the ladder on";
    layers.setPrimaryField(2);
    EXPECT_TRUE(layers.wallRadarEnabled());
    layers.setLayerEnabled("wallRadar", false);
    EXPECT_FALSE(layers.wallRadarEnabled());
    EXPECT_EQ(events, (std::vector<std::string>{"wallRadar+", "primaryField-", "wallRadar-"}));
    changes = 0;
    layers.setWallMultiple(0.5);
    EXPECT_DOUBLE_EQ(layers.wallMultiple(), chart::LayerVisibility::kWallMinMultiple);
    layers.setWallMultiple(100);
    EXPECT_DOUBLE_EQ(layers.property("wallMultiple").toDouble(), chart::LayerVisibility::kWallMaxMultiple);
    layers.setWallMultiple(std::nan(""));
    layers.setWallMultiple(chart::LayerVisibility::kWallMaxMultiple);
    EXPECT_DOUBLE_EQ(layers.wallMultiple(), 20);
    EXPECT_EQ(changes, 2) << "only real changes notify";
}
} // namespace
