#pragma once
// GPU upload image of one composed heatmap::SparseColumns (one timeframe).
// Built on a worker thread, immutable afterwards, shared with the render thread.
//
// Precision contract (matches heatmap::binColumn, the CPU reference):
// - Each output column reads exactly ONE composed source column (time is composed
//   on the CPU by heatmap::TimeComposer; the GPU never sums across time).
// - Constituents that share a native tick are pooled exactly as binColumn pools
//   them (heatmap::detail::aggregateRows). Each native row/side value is
//   numerator / pooled covered duration * (group observed / column observed),
//   computed in double on the CPU. No price bins are pre-summed.
// - Values travel as float-float (hi float + compact low part, ~2^-38 relative),
//   so GPU sums reproduce the CPU double sums closely enough to give identical
//   15-bit log codes.
#include "heatmap/SparseColumns.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace heatmap::gpu {

inline constexpr uint32_t kSlotNotLoaded = 0xffffffffu; // bucket not scanned yet
inline constexpr uint32_t kSlotGap = 0xfffffffeu;       // scanned, recorder had no column
inline constexpr uint32_t kMaxTicks = 16;               // distinct native ticks per source
inline constexpr uint32_t kRowIndexStride = 16;         // native rows per row-index step

// std430 layout shared with heatmap_bin.comp. Rows are absolute native rows on
// the group's tick (price = row * tick).
struct GroupMeta {
    int32_t baseRow = 0;      // entry row 0 == this absolute native row
    uint32_t entryBegin = 0, entryEnd = 0;
    uint32_t tickIndex = 0;   // into GpuSource::ticks
    uint32_t runBegin[2] = {0, 0}, runEnd[2] = {0, 0}; // full-coverage runs, bid=0 ask=1
    // rowIndex[indexBegin + s] = first entry with relative row >= s * kRowIndexStride,
    // so a bin's first entry costs one lookup plus a search of <= 16 rows of entries.
    uint32_t indexBegin = 0, indexCount = 0;
    uint32_t reserved[2] = {0, 0};
};
static_assert(sizeof(GroupMeta) == 48);

struct GpuSourceOptions {
    // Advertised availability. Buckets outside [availableStartMs, availableEndMs)
    // draw "no data"; inside it, a bucket the source does not cover draws "loading".
    // Unset: the source's bounding extent.
    std::optional<int64_t> availableStartMs, availableEndMs;
    // Row clip: keep native rows whose price lies in [priceLo, priceHi). Display
    // bins not fully inside draw "loading" (their rows were not uploaded).
    std::optional<double> priceLo, priceHi;
};

struct GpuSource {
    uint64_t id = 0; // process-unique, changes whenever content changes
    std::string symbol, layer;
    int64_t tfMs = kMinuteMs;
    int64_t firstBucket = 0; // absolute bucket index (ms / tfMs) of bucketSlots[0]
    int64_t availableFirstBucket = 0, availableEndBucket = 0;
    double clipPriceLo = -std::numeric_limits<double>::infinity();
    double clipPriceHi = std::numeric_limits<double>::infinity();
    std::vector<double> ticks;           // distinct native ticks in price units
    double priceScale = 100;             // native grid price scale (ladder units per price unit)
    std::vector<uint32_t> bucketSlots;         // per bucket: column index, kSlotNotLoaded or kSlotGap
    std::vector<uint32_t> columnGroups;  // prefix offsets into groups, columns + 1
    std::vector<GroupMeta> groups;
    std::vector<std::array<int32_t, 2>> runs; // [lo, hi] absolute native rows, inclusive
    std::vector<uint32_t> rowIndex;           // see GroupMeta::indexBegin
    // Compact (2 words per entry):  [hi float bits, side | rel row << 1 | loQ << 17]
    // Wide    (3 words per entry):  [hi float bits, lo float bits, side | rel row << 1]
    // Wide is used only when a group's relative row span exceeds 16 bits.
    bool wide = false;
    std::vector<uint32_t> entries;
    uint64_t entryCount = 0;
    // Covered price extent over all groups (for initial views); NaN if empty.
    double coveredPriceLo = std::numeric_limits<double>::quiet_NaN();
    double coveredPriceHi = std::numeric_limits<double>::quiet_NaN();
    size_t columns() const { return columnGroups.empty() ? 0 : columnGroups.size() - 1; }
    uint32_t wordsPerEntry() const { return wide ? 3u : 2u; }
    uint64_t bytes() const {
        return bucketSlots.size() * 4ull + columnGroups.size() * 4ull + groups.size() * sizeof(GroupMeta) +
               runs.size() * 8ull + rowIndex.size() * 4ull + entries.size() * 4ull;
    }
};

// Throws std::invalid_argument when the data cannot be represented (rows beyond
// int32, more than kMaxTicks native ticks, more than 2^32 entries). Input must
// already be composed at its timeframe and pass heatmap::validate().
GpuSource buildGpuSource(const SparseColumns& columns, const GpuSourceOptions& options = {});

// Float-float helpers shared by the builder and tests.
struct FloatFloat { float hi = 0, lo = 0; };
FloatFloat splitDouble(double value);
// Compact low part: lo quantized to ulp(hi) * 2^-14 as a 15-bit signed integer.
int32_t quantizeLow(float hi, float lo);
float dequantizeLow(float hi, int32_t q);

// Smallest double v with recording::encodeSize(v, scale) >= k, for k = 2..kMaxCode,
// split as float-float (hi, lo) pairs: index k - 2. A value v > 0 then encodes to
// 1 + (number of thresholds <= v). Exact against the CPU encoder by construction.
std::vector<FloatFloat> encodeThresholds(const recording::SizeScale& scale);

// Smallest display tick every native grid of the source can build (LCM of the
// native ticks in price units). Display-tick policy must pick multiples of it,
// or columns on the other grid veil as incompatible. 0 if there are no ticks.
double commonTick(const GpuSource& source);

// Display tick -> per-tick native rows per display bin (0 = incompatible grid),
// the same integrality rule binColumn applies.
std::array<uint32_t, kMaxTicks> tickFactors(const GpuSource& source, double displayTick);

} // namespace heatmap::gpu
