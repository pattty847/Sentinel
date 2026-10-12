#pragma once
#include <utility>

namespace chart {
// Keep the gate closed unless the complete frame can be submitted.
template<class Prepare, class Gate, class Tile, class Frame>
bool prepareHeatmap(bool draw, bool valid, Prepare prepare, Gate& gate, Tile& tile, Frame& frame) {
    const bool prepared = draw && valid && prepare(frame);
    const double opacity = prepared ? 1.0 : 0.0;
    if (gate.opacity() != opacity) gate.setOpacity(opacity);
    if (prepared) tile.setFrame(std::move(frame));
    return prepared;
}

template<class Text, class Begin, class Axis, class Labels, class Mark>
void submitChartText(Text& text, Begin begin, Axis axis, Labels labels, Mark mark) {
    begin();
    axis();
    mark(0);
    labels();
    mark(1);
    text.endFrame();
    mark(2);
}
// Outside sync: consume atomics only; the callbacks enqueue GUI work.
template<class Stats, class Incomplete, class QueueUpdate, class QueueLabels>
void finishHeatmapFrame(const Stats& stats, Incomplete& incomplete,
                        const QueueUpdate& update, const QueueLabels& labels) {
    if (stats && stats->wantsFrame.exchange(false)) update();
    if (incomplete.exchange(false)) labels();
}
} // namespace chart
