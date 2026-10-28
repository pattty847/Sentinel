#include "chart/core/LiveEdge.hpp"
#include "chart/core/ChartSession.hpp"
#include "chart/core/ChartPorts.hpp"
#include "servermodel/RecordingCodec.hpp"

namespace chart {
int64_t LiveEdge::bucketEnd(int64_t timeMs, int64_t tfMs) {
    return recording::floorDiv(timeMs, tfMs) * tfMs + tfMs;
}

int64_t LiveEdge::anchorMs(const ChartSession& session, const IChartEnvironment& environment, int64_t tfMs) {
    if (!environment.attached() || tfMs <= 0) return 0;
    return tfMs >= kMinuteMs ? environment.liveAnchorMs() : session.lastTradeMs;
}

int64_t LiveEdge::endMs(const ChartSession& session, const IChartEnvironment& environment, int64_t tfMs) {
    const int64_t anchor = anchorMs(session, environment, tfMs);
    if (anchor <= 0) return 0;
    return tfMs >= kMinuteMs ? recording::floorDiv(anchor + tfMs - 1, tfMs) * tfMs : bucketEnd(anchor, tfMs);
}
} // namespace chart
