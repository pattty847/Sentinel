#pragma once
// S7b: which label columns describe exactly the picture on screen. Pure; render
// thread (updatePaintNode) or GUI thread.
//
// A column of a LabelCells result is matched when
// - the node drew its bucket in its last frame (layer 0) from the same content:
//   a span piece whose spanContentId equals the one the labels were built from
//   (0 = a partial bin: never), or the live bin with the labels' live version,
//   and the labels took that bucket from the same kind (span or live);
// - and this frame's target picture still has that content: the node's prepare()
//   runs after the label layout and may switch to the target in this very frame
//   (a new live version paged in, a span's new revision ready).
// Unmatched columns draw no labels until both sides agree.
#include "HeatmapCellQuery.hpp"
#include "HeatmapTileNode.hpp"
#include <vector>

namespace heatmap::gpu {
// out: one byte per label column (1 matched). Returns the matched count. No
// allocation once `out` has the capacity (16,000 columns at most).
size_t matchLabelColumns(const LabelCells &labels, const std::vector<HeatmapTileStats::Segment> &drawn,
                         const SpanSet *target, const LiveSnapshot *targetLive, std::vector<uint8_t> &out);
} // namespace heatmap::gpu
