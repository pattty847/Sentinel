#pragma once
#include "SparseColumns.hpp"
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
// NotLoaded from a proven recorder Gap. Incomplete output aggregates are omitted.
SparseColumns compose(std::span<const SparseColumns> levelColumns, int64_t tfMs);
inline SparseColumns compose(const SparseColumns& columns, int64_t tfMs) {
    return compose(std::span<const SparseColumns>(&columns, 1), tfMs);
}
} // namespace heatmap
