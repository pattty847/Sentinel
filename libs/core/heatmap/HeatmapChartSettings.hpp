#pragma once
// Persistable chart values, no Qt/GUI types. Tick values are integer price units
// (price * source priceScale), the same units as ManualTickMemory.
#include "HeatmapResolution.hpp"
#include <string>
#include <vector>

namespace heatmap {
struct GradientStop {
    double position = 0;
    std::string color = "#000000"; // #RRGGBB or #RRGGBBAA
    bool operator==(const GradientStop &) const = default;
};
struct HeatmapChartSettings {
    std::string renderer = "gpu";
    TickMode tickMode = TickMode::Auto;
    int64_t manualTick = 100;
    double minRowPx = 2, hysteresis = 0.25;
    int crossfadeMs = 150;
    bool showBandEdges = false;
    std::string palettePreset = "Electric"; // five legacy presets, or Custom
    std::vector<GradientStop> bidGradient{{0, "#000000"}, {1, "#00ffff"}};
    std::vector<GradientStop> askGradient{{0, "#000000"}, {1, "#ffc800"}};
    double sensitivityMin = 0.05, sensitivityMax = 50, opacity = 1;
    uint64_t gpuCapBytes = 320ull << 20, uploadBudgetBytes = 8ull << 20;
    int prefetchTiles = 1, liveMinIntervalMs = 500;
    bool showTelemetry = false;
    // Liquidity labels (S7b, owner decision 1/2): on every coloured cell where the
    // text fits at labelMinPx plus padding, growing to at most labelMaxPx; USD
    // (price x size) or the asset amount ("usd" or "asset").
    bool showLabels = true;
    std::string labelCurrency = "usd";
    double labelMinPx = 12, labelMaxPx = 15;
    bool operator==(const HeatmapChartSettings &) const = default;
};
} // namespace heatmap
