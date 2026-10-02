#pragma once
// Multi-source span planning and the resolution summary (plan
// docs/research/2026-09-s5-plan.md sections 2-3, slice S5b). Extends HeatmapTiles
// (chunksFor, tilesCovering) from one source to several per span.
// - A span is one epoch-aligned tile (tiles::kTileColumns columns of the tf).
//   Each span keeps one source per source id; the node bins the coarsest source
//   first and fills only its veiled cells with finer ones (owner decision 1).
// - Source ids are data (ChunkCodec kChunkSources). Nothing here branches on them.
// - Tick selection over the summary reuses the slice-T policy in
//   HeatmapResolution.hpp (autoTickUnitsIf, manualPresetUnits).
// Pure functions and value types; any thread; no Qt.
#include "HeatmapResolution.hpp"
#include "HeatmapTiles.hpp"
#include <compare>

namespace heatmap {
// ------------------------------------------------------------------ spans
// Lower rank wins (fetch, build and admission order; eviction takes the highest).
// Visible: tiles the view touches. Fallback: the previous tf's visible spans while
// the current tf's visible spans are still building (never evicted). Label:
// optional cell-query reloads, below the drawn picture. Prefetch:
// ordered by tile distance from the view. RecentTf: the previous tf's visible
// spans once the current view is complete (evicted first).
enum class SpanTier : uint8_t { Visible, Fallback, Label, Prefetch, RecentTf };
const char *spanTierName(SpanTier tier);
struct SpanRank {
    SpanTier tier = SpanTier::Visible;
    // Visible: tiles from the view's centre tile (0 = centre), so the spans
    // nearest the centre come first (and are refused last under the CPU
    // ceiling). Prefetch: tiles outside the view (1 = adjacent).
    int64_t distance = 0;
    auto operator<=>(const SpanRank &) const = default;
    // ChunkFetcher priority: greater wins, strictly monotonic in rank.
    int fetchPriority() const;
};
struct SpanId {
    std::string symbol;
    int64_t tfMs = 0, tile = 0;
    auto operator<=>(const SpanId &) const = default;
    int64_t startMs() const { return tiles::tileStartMs(tile, tfMs); }
    int64_t endMs() const { return tiles::tileEndMs(tile, tfMs); }
};
struct SourceAvailability {
    std::string source; // chunk source id
    tiles::Availability time;
};
struct SpanSourcePlan {
    std::string source;
    // The source's availability clipped to the span: a span entirely in the past
    // keeps the same bounds (and build key) while the live edge advances.
    int64_t availableStartMs = 0, availableEndMs = 0;
    std::vector<ChunkKey> chunks; // ascending (tiles::chunksFor)
};
struct PlannedSpan {
    SpanId id;
    SpanRank rank;
    std::vector<SpanSourcePlan> sources; // in the order of the availability input
};
// Prefetch tiles per side: at least 2, or one view width (B1: 1 tile measured a
// p95 of 56-59 ms on small horizontal pans).
int64_t prefetchTiles(double timeLoMs, double timeHiMs, int64_t tfMs);
// Visible spans by distance from the view centre, then prefetch by distance
// (left before right at equal distance),
// then `retained` spans (other timeframes; they keep their own rank). A span with
// no chunk of any source (outside every source's availability) is left out.
// Sorted by rank; an id that appears twice keeps its best rank.
std::vector<PlannedSpan> planSpans(const std::string &symbol, int64_t tfMs, double timeLoMs, double timeHiMs,
                                   std::span<const SourceAvailability> sources,
                                   std::span<const PlannedSpan> retained = {});

// ------------------------------------------------------------------ resolution
// Integer price units of ResolutionSummary::priceScale (as HeatmapResolution).
struct PriceRange {
    int64_t lo = 0, end = 0; // half-open
    bool operator==(const PriceRange &) const = default;
};
struct SourceResolution {
    std::string source;
    BucketState state = BucketState::NotLoaded;
    std::vector<int64_t> nativeTicks; // distinct, ascending (price units)
    int64_t commonUnits = 0;          // LCM of nativeTicks: the finest tick it builds
    // Rows with full bid AND ask coverage for the whole column, intersected over
    // every native constituent (binColumn's validity rule). Sorted, disjoint;
    // holes stay holes. For a banded source (today near) this is its band.
    std::vector<PriceRange> bands;
};
struct ColumnResolution {
    int64_t startMs = 0;
    std::vector<SourceResolution> sources; // present, gap or not-loaded sources
};
// Tick-free: native ticks and band extents per column and source.
struct ResolutionSummary {
    int64_t tfMs = 0;
    double priceScale = 100;
    std::vector<ColumnResolution> columns; // ascending startMs
};
// One source's columns in [startMs, endMs) (multiples of tfMs), composed at tfMs.
ResolutionSummary summarizeResolution(const SparseColumns &composed, const std::string &source, int64_t tfMs,
                                      int64_t startMs, int64_t endMs, double priceScale = 100);
// Adds `from`'s sources to the matching columns of `into` (same tf and scale).
void mergeResolution(ResolutionSummary &into, const ResolutionSummary &from);

// Rows of one column that tick T cannot build although some source has full
// coverage there: the column's data rows (union of every source's bands, whole
// T bins) minus the rows a source whose commonUnits divides T covers (whole T
// bins). Rows no source covers (outside the book, holes) are not counted: they
// veil at every tick, so they never block a tick. Limited to [lo, end).
std::vector<PriceRange> unbuildableRows(const ColumnResolution &column, int64_t tickUnits, PriceRange rows);
struct VeiledRange {
    int64_t startMs = 0, endMs = 0; // consecutive columns with the same rows
    PriceRange price;
    bool operator==(const VeiledRange &) const = default;
};
// Resolution-indicator data for a (locked) tick: the columns and price ranges in
// view that draw the veil because no source builds the tick there. The view is
// widened to whole absolute tick bins (a partial edge row counts).
std::vector<VeiledRange> veiledRanges(const ResolutionSummary &summary, int64_t tickUnits, double timeLoMs,
                                      double timeHiMs, double priceLo, double priceHi);
// True when some loaded column in the time range has a source whose common
// tick divides tickUnits, and no column in view has unbuildable rows in the
// price range (owner decision 1: judged on the rows in view).
bool buildsInView(const ResolutionSummary &summary, int64_t tickUnits, double timeLoMs, double timeHiMs,
                  double priceLo, double priceHi);
// Auto tick over the summary: autoTickUnitsIf (slice-T hysteresis) restricted to
// presets that buildsInView. heightPx is physical pixels. 0 when nothing builds.
int64_t autoTickUnits(const ResolutionSummary &summary, int64_t currentUnits, double timeLoMs, double timeHiMs,
                      double priceLo, double priceHi, double heightPx, const AutoTickParams &params = {});
// Manual presets (<= maxUnits): every preset some loaded column of some source
// with coverage can build (spec rule 4, option B).
std::vector<int64_t> manualPresetUnits(const ResolutionSummary &summary, int64_t maxUnits);
} // namespace heatmap
