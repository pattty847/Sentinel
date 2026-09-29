#pragma once
#include "SparseColumns.hpp"

namespace heatmap {
struct BinCell {
    double bid = 0, ask = 0;
    bool valid = false, dominantAsk = false;
    uint16_t code = 0; // dominant size on the requested scale, including the side bit
};
// Bin [priceLo, priceLo + displayTick) on the absolute display grid. Choose the
// display tick with PriceLadder/HeatmapResolution. Incompatible native grids make
// the cell unknown; covered zero rows remain valid, and ties select bid.
BinCell binCell(const SparseColumn& column, double priceLo, double displayTick,
                const recording::SizeScale& outputScale = {});
// Outward-align a price range to displayTick and return cells in descending price
// order, the same lower-edge convention as recording::buildPage.
std::vector<BinCell> binColumn(const SparseColumn& column, double priceLo, double priceHi,
                              double displayTick, const recording::SizeScale& outputScale = {});
} // namespace heatmap
