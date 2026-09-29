#pragma once
#include "SparseColumns.hpp"
#include "../servermodel/Hmc2Store.hpp"

namespace heatmap {
// HMC2 adapter for tests/lab and future chunk builders; model/composer have no
// reader ownership. Conversion retains original codes, scale, flags and grid id.
SparseColumn fromRecording(const recording::Hmc2Record& record);
SparseColumns loadRecording(recording::Hmc2Reader& reader, const std::string& symbol,
                             const std::string& layer, int64_t levelMs, int64_t startMs, int64_t endMs);
// Range must contain complete output buckets. Hour multiples use deep hours and
// only the newest unpersisted hour's minute tail, matching buildPage. All other
// timeframes (including 16m/90m) use minutes. Resolve layerFor() before calling.
std::vector<SparseColumns> loadRecordingLevels(recording::Hmc2Reader& reader, const std::string& symbol,
    const std::string& layer, int64_t startMs, int64_t endMs, int64_t tfMs);
} // namespace heatmap
