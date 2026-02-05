/*
Sentinel — TimeAxisMapping
Role: Single source of truth for world-time/price → screen mapping.
      All chart layers (heatmap, candles, labels, future TPO/footprint)
      consume the same mapping. Core invariant: 1 slice = 1 candle.
Threading: Populated on render thread in updatePaintNode(); consumed by
           all renderers in the same frame.
*/
#pragma once

#include <QRectF>
#include <cmath>
#include <cstdint>

struct TimeAxisMapping {
    // View bounds (pan baked in during drag)
    double viewStartMs = 0.0;
    double viewEndMs = 0.0;
    double viewMinPrice = 0.0;
    double viewMaxPrice = 0.0;

    // Data window (ring buffer logical range)
    double dataStartMs = 0.0;
    double dataEndMs = 0.0;
    double actualDataStartMs = 0.0;
    double actualDataEndMs = 0.0;
    double dataMinPrice = 0.0;
    double dataMaxPrice = 0.0;

    // Grid params
    double appendMs = 0.0;    // timeframe per bucket (ms)
    double tickSize = 0.0;
    int gridWidth = 0;
    int gridHeight = 0;
    int filledColumns = 0;

    // Screen geometry (computed from overlap)
    QRectF drawRect;          // screen sub-rect where data is visible
    QRectF srcRect;           // texture column/row sub-rect
    double cellW = 0.0;       // pixels per column
    double cellH = 0.0;       // pixels per row

    // Heatmap shader ONLY — never use for candle/label mapping
    float timeOffset = 0.0f;

    bool valid = false;

    // --- World → Screen helpers ---

    // Maps a world-time timestamp to screen X via srcRect → drawRect interpolation.
    // Does NOT incorporate timeOffset (that's shader-only).
    double timeToScreenX(double timeMs) const {
        if (!valid || srcRect.width() <= 0.0 || appendMs <= 0.0)
            return 0.0;
        const double colF = (timeMs - dataStartMs) / appendMs;
        const double srcFrac = (colF - srcRect.x()) / srcRect.width();
        return drawRect.x() + srcFrac * drawRect.width();
    }

    // Maps a price to screen Y via srcRect → drawRect interpolation.
    double priceToScreenY(double price) const {
        if (!valid || srcRect.height() <= 0.0 || tickSize <= 0.0)
            return 0.0;
        const double rowF = (dataMaxPrice - price) / tickSize;
        const double srcFrac = (rowF - srcRect.y()) / srcRect.height();
        return drawRect.y() + srcFrac * drawRect.height();
    }

    // Inverse: screen X → world time
    double screenXToTime(double x) const {
        if (!valid || drawRect.width() <= 0.0 || srcRect.width() <= 0.0 || appendMs <= 0.0)
            return 0.0;
        const double srcFrac = (x - drawRect.x()) / drawRect.width();
        const double colF = srcRect.x() + srcFrac * srcRect.width();
        return dataStartMs + colF * appendMs;
    }

    // Inverse: screen Y → price
    double screenYToPrice(double y) const {
        if (!valid || drawRect.height() <= 0.0 || srcRect.height() <= 0.0 || tickSize <= 0.0)
            return 0.0;
        const double srcFrac = (y - drawRect.y()) / drawRect.height();
        const double rowF = srcRect.y() + srcFrac * srcRect.height();
        return dataMaxPrice - rowF * tickSize;
    }

    // --- Bucket helpers ---

    // Fractional column in data space for a given timestamp.
    double timeToColumnF(double timeMs) const {
        if (appendMs <= 0.0) return 0.0;
        return (timeMs - dataStartMs) / appendMs;
    }

    // Epoch-aligned bucket start for a given timestamp.
    int64_t bucketStartMsForTime(double timeMs) const {
        if (appendMs <= 0.0) return 0;
        const int64_t t = static_cast<int64_t>(timeMs);
        const int64_t tf = static_cast<int64_t>(appendMs);
        return (t / tf) * tf;
    }

    // Column index in ring (0..gridWidth-1) for a given timestamp.
    int bucketIndexForTime(double timeMs) const {
        if (appendMs <= 0.0 || gridWidth <= 0) return 0;
        const double col = (timeMs - dataStartMs) / appendMs;
        int idx = static_cast<int>(std::floor(col)) % gridWidth;
        if (idx < 0) idx += gridWidth;
        return idx;
    }

    // Pixels per column.
    double bucketWidthPx() const { return cellW; }

    // --- Visible range ---

    double visibleDataStartMs() const {
        return std::max(viewStartMs, actualDataStartMs);
    }

    double visibleDataEndMs() const {
        return std::min(viewEndMs, actualDataEndMs);
    }
};
