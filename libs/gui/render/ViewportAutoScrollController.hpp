/*
Sentinel — ViewportAutoScrollController
Role: Owns auto-scroll lag/span math and applies viewport updates via GridViewState.
Threading: GUI thread only.
*/
#pragma once

#include <cstdint>
#include <limits>

class GridViewState;
class HeatmapStreamState;

class ViewportAutoScrollController {
public:
    void setPaddingFrac(double fraction);
    void setSmoothEnabled(bool enabled);
    bool smoothEnabled() const { return m_smoothEnabled; }

    void resetSpan();
    void updateLagFromView(const GridViewState& view, const HeatmapStreamState& stream);
    void updateLagFromView(const GridViewState& view, const HeatmapStreamState& stream, int64_t timeframeMs);
    bool initializeViewport(GridViewState& view,
                            const HeatmapStreamState& stream,
                            int64_t sliceStartMs,
                            int timeframeMs);
    bool applySliceAutoScroll(GridViewState& view,
                              const HeatmapStreamState& stream,
                              int64_t sliceStartMs,
                              int timeframeMs);
    bool applySmoothAutoScroll(GridViewState& view,
                               const HeatmapStreamState& stream,
                               int64_t nowMs,
                               int64_t timeframeMs);
    bool applyPanToAutoScroll(GridViewState& view,
                              double viewportWidth,
                              double viewportHeight);

private:
    int64_t m_autoScrollLagMs = 0;
    int64_t m_autoScrollSpanMs = 0;
    int64_t m_lastViewEndMs = std::numeric_limits<int64_t>::min();
    double m_paddingFrac = 0.05;
    bool m_smoothEnabled = true;
};
