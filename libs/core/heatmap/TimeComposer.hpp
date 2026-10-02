#pragma once
#include "SparseColumns.hpp"
#include <optional>
#include <span>

namespace heatmap {
// Select the coarsest supplied level that divides tfMs exactly. Coarser scanned
// ranges supersede finer ranges INCLUDING gaps, so persisted hours are never
// counted twice or silently backfilled with minutes. Supply minutes outside the
// sealed hour range for the open tail. Nondividing levels (e.g. hours for 90m)
// are ignored; the caller must supply minute coverage for those timeframes.
// Same-level scan ranges must not overlap. Throws on malformed/incompatible data.
// scannedRanges proves ONLY complete output buckets; bounding start/end includes
// unloaded holes and partial edge buckets. Query bucketState() to distinguish
// NotLoaded from a proven recorder Gap. Incomplete output aggregates are omitted
// unless forming is requested; then the terminal bucket proves only its known
// prefix and carries that prefix's observed duration.
struct ComposeOptions {
    // Output range clip [startMs, endMs), both multiples of tfMs: only buckets in
    // it are composed and proven scanned (B1 tiles compose a tile span out of the
    // chunks that overlap it). Unset: the union of the inputs.
    std::optional<int64_t> startMs, endMs;
    // Physical price window [lo, end). Retain the native rows intersecting it,
    // including coverage of zero rows and empty/incompatible native grids.
    // Binning cells wholly inside this window is identical to an unclipped
    // compose. Tick independent: several display ticks can reuse the result.
    struct PriceClip {
        double lo = 0, end = 0;
        bool operator==(const PriceClip &) const = default;
    };
    std::optional<PriceClip> price;
    // Skip validate() of inputs and output. Only for inputs that were validated
    // once already (ChunkStore validates every chunk it loads).
    bool trustedInputs = false;
    // Keep the terminal bucket when its entire available prefix is scanned.
    // Future time is excluded from observedMs; an interior unloaded hole still
    // makes the bucket NotLoaded. Default history composition is unchanged.
    bool forming = false;
};
// Combine disjoint observed pieces of one bucket, including cached composed
// prefixes. Preserves unquantized numerators and row coverage. Inputs are trusted
// and the caller separately proves the time scan (no synthetic missing time).
SparseColumn composeColumn(std::span<const SparseColumn* const> columns, int64_t startMs, int64_t tfMs);
// Several non-overlapping inputs of one level (e.g. consecutive hour chunks of
// minutes) are allowed; they are treated as one level.
SparseColumns compose(std::span<const SparseColumns* const> levelColumns, int64_t tfMs,
                      const ComposeOptions& options = {});
SparseColumns compose(std::span<const SparseColumns> levelColumns, int64_t tfMs);
inline SparseColumns compose(const SparseColumns& columns, int64_t tfMs) {
    return compose(std::span<const SparseColumns>(&columns, 1), tfMs);
}
} // namespace heatmap
