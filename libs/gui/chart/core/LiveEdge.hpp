#pragma once
#include <cstdint>

namespace chart {
struct ChartSession;
class IChartEnvironment;

// Raw timestamps and exclusive bucket ends are deliberately separate. Minute
// anchors already describe an exclusive heatmap end; trades describe events.
struct LiveEdge {
    static constexpr int64_t kMinuteMs = 60000;
    static int64_t bucketEnd(int64_t timeMs, int64_t tfMs);
    static int64_t anchorMs(const ChartSession& session, const IChartEnvironment& environment, int64_t tfMs);
    static int64_t endMs(const ChartSession& session, const IChartEnvironment& environment, int64_t tfMs);
};
} // namespace chart
