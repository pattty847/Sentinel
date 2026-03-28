#pragma once

#include <QPointF>
#include <QRectF>
#include <cstdint>

#include "HeatmapStreamState.hpp"
#include "TimeAuthority.hpp"
#include "TimeAxisMapping.hpp"

/// Per-frame viewport snapshot captured from GridViewState.
struct FrameViewportSnapshot {
    bool valid = false;
    qint64 timeStart = 0;
    qint64 timeEnd = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    QPointF panVisualOffset;
    bool dragging = false;
    bool autoScrollEnabled = false;
};

/// Per-frame stream generation counters for change detection.
struct FrameStreamGenerations {
    uint64_t heatmap = 0;
    uint64_t footprint = 0;
    uint64_t candle = 0;
};

/// Immutable per-frame snapshot assembled at the start of updatePaintNode.
struct FrameContext {
    struct OverlayActivationSet {
        bool heatmap = false;
        bool footprint = false;
        bool tpo = false;
    };
    TimeAuthority::Snapshot time;
    QRectF surfaceBounds;
    double surfaceDpr = 1.0;
    qint64 presentationTimeMs = 0;
    FrameViewportSnapshot viewport;
    HeatmapStreamState::Snapshot heatmapSnapshot;
    FrameStreamGenerations streamGenerations;
    OverlayActivationSet overlays;
    bool forceFull = false;
    TimeAxisMapping mapping;
};
