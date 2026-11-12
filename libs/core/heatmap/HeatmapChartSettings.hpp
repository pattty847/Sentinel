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
    TickMode tickMode = TickMode::Auto;
    int64_t manualTick = 100;
    double minRowPx = 2, hysteresis = 0.25;
    int crossfadeMs = 150;
    bool showBandEdges = false;
    std::string palettePreset = "Electric"; // five legacy presets, or Custom
    std::vector<GradientStop> bidGradient{{0, "#000000"}, {1, "#00ffff"}};
    std::vector<GradientStop> askGradient{{0, "#000000"}, {1, "#ffc800"}};
    double sensitivityMin = 0.05, sensitivityMax = 50, opacity = 1; // active symbol, base units
    std::string sensitivitySymbol; // active symbol; empty for direct layer callers
    bool sensitivityReady = true;  // false until an unsaved symbol has a price
    bool smoothZoomOut = false;
    uint64_t gpuCapBytes = 320ull << 20, uploadBudgetBytes = 8ull << 20;
    int prefetchTiles = 1, liveMinIntervalMs = 500;
    bool showTelemetry = false;
    bool showTrades = false;
    bool tradesAboveCandles = true;
    double tradeMinNotional = 0; // 0 all, -1 visible median, >0 fixed quote minimum
    std::string bubblePlacement = "individual";
    // Liquidity labels (S7b, owner decision 1/2): on every coloured cell where the
    // text fits at labelMinPx plus padding, growing to at most labelMaxPx; USD
    // (price x size) or the asset amount ("usd" or "asset").
    bool showLabels = true;
    std::string labelCurrency = "usd";
    double labelMinPx = 12, labelMaxPx = 15;
    std::string candleUpColor = "#2EBD85", candleDownColor = "#F6465D";
    std::string candleWickColor = "auto"; // auto follows each candle's body color
    double candleBodyOpacity = 1;
    int candleWickWidth = 1; // device pixels
    double candleBodyFraction = 0.85;
    int candleMaxBodyPx = 71; // logical pixels
    // Cumulative volume delta strip; height in logical px, 0..400 (0 disables).
    bool flowFooter = false;
    int cvdStripPx = 0;
    std::string cvdReset = "visible", cvdStyle = "line";
    // Flow-backed footprint presentation.
    std::string footprintIntensity = "bar";
    std::string footprintCandle = "spine";
    double footprintDustNotional = 2500;
    bool footprintNumbers = true;
    // Chart-edge book ladder strip (chart menu); width in logical px, 60..240.
    bool showLadder = false;
    int ladderWidthPx = 120;
    // Wall radar on the ladder (chart menu): bins at or above wallMultiple x the side's
    // median row size are drawn as walls; 1.5..20.
    bool showWallRadar = false;
    double wallMultiple = 4;
    bool operator==(const HeatmapChartSettings &) const = default;
};
} // namespace heatmap
