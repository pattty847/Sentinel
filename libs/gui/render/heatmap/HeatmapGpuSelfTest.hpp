#pragma once
// Runtime precision self-test for the GPU binning kernel. The fast kernel keeps
// float-float error terms alive under Metal fast math by an observed-behaviour
// trick (XOR laundering); this fixture proves it on the actual driver before the
// fast path is trusted. Pure CPU (no Qt GUI): the fixture, its binColumn oracle
// and the comparison. HeatmapGpuBinner runs it once per QRhi backend/device.
#include "HeatmapBinGrid.hpp"
#include "HeatmapGpuSource.hpp"
#include <vector>

namespace heatmap::gpu {
struct PrecisionSelfTest {
    SparseColumns data;
    GpuSource source;
    BinGrid grid;
    std::vector<uint32_t> expected; // cell words (row 0 = top), from binColumn
};
// ~400 cells: bins whose exact sum lies 1e-9 above a code threshold while the
// plain float sum of the same entries stays below it (every addition's
// rounding error must survive), plus a block of ordinary random bins.
PrecisionSelfTest makePrecisionSelfTest();
// Cells whose code, side or state differ from the oracle.
size_t countSelfTestMismatches(const PrecisionSelfTest &test, const std::vector<uint32_t> &cells);
} // namespace heatmap::gpu
