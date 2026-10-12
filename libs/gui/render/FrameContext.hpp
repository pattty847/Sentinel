#pragma once

#include <QPointF>
#include <QRectF>
#include <cstdint>

#include "ChartRaster.hpp"
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
    // GridViewState::rasterAnchor (the raster camera's fixed point).
    double anchorFracX = 0.5;
    double anchorFracY = 0.5;
};

/// Immutable per-frame snapshot assembled at the start of updatePaintNode.
struct FrameContext {
    uint64_t controlRevision = 0;
    uint64_t selectionEpoch = 0;
    uint64_t viewportVersion = 0;
    uint64_t frameId = 0;
    struct OverlayActivationSet {
        bool heatmap = false;
        bool footprint = false;
        bool tpo = false;
        bool volumeProfile = false;
    };
    TimeAuthority::Snapshot time;
    QRectF itemBounds; // unsnapped item extent for axis text
    QRectF surfaceBounds;
    double surfaceDpr = 1.0;
    qint64 presentationTimeMs = 0;
    FrameViewportSnapshot viewport;
    OverlayActivationSet overlays;
    TimeAxisMapping mapping;
    chart_raster::RasterCamera raster; // what this frame draws (mapping is toMapping(raster))
};
