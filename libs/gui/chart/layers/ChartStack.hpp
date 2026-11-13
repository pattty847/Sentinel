#pragma once

namespace chart {
// Child-item depths within one chart. Layer visibility never changes this order.
enum class ChartDepth { Fields = 0, BubblesBelow = 1, Candles = 2, Profiles = 3, BubblesAbove = 4, Text = 5 };
enum class LayerPlane { Fields, Profiles, Foreground };
constexpr int depth(ChartDepth value) { return static_cast<int>(value); }
} // namespace chart
