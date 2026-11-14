#pragma once

namespace chart {
// Child-item depths within one chart. Layer visibility never changes this order.
enum class ChartDepth { Fields = 0, Volume = 1, BubblesBelow = 2, Candles = 3, Profiles = 4, BubblesAbove = 5, Text = 6 };
enum class LayerPlane { Fields, Profiles, Foreground };
constexpr int depth(ChartDepth value) { return static_cast<int>(value); }
} // namespace chart
