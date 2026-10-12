#include "chart/shell/LayerVisibility.hpp"
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
} // namespace
