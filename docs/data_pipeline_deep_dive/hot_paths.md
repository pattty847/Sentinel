# Hot Path Reference

All snippets are verbatim from the current codebase with comments describing how they touch memory.

## 1. Slice Scheduling (`DataProcessor::updateVisibleCells`)

```cpp
// libs/gui/render/DataProcessor.cpp:486-517
if (viewportChanged || m_lastProcessedTime == 0) {
    m_processedTimeRanges.clear();
    for (const auto* slice : visibleSlices) {
        ++processedSlices;
        createCellsFromLiquiditySlice(*slice);
        m_processedTimeRanges.insert({slice->startTime_ms, slice->endTime_ms});
        if (slice && slice->endTime_ms > m_lastProcessedTime) {
            m_lastProcessedTime = slice->endTime_ms;
        }
    }
} else {
    for (const auto* slice : visibleSlices) {
        if (!slice) continue;
        SliceTimeRange range{slice->startTime_ms, slice->endTime_ms};
        if (m_processedTimeRanges.find(range) == m_processedTimeRanges.end()) {
            ++processedSlices;
            createCellsFromLiquiditySlice(*slice);
            m_processedTimeRanges.insert(range);
        }
        if (slice->endTime_ms > m_lastProcessedTime) {
            m_lastProcessedTime = slice->endTime_ms;
        }
    }
}
```

* **Iteration count:** `visibleSlices` is whatever the LTE returns for the viewport window; at 100 ms buckets over a 30 s view this is ≈300 slices per frame. The loop is sequential, touching two cache-friendly containers: `std::vector<const LiquidityTimeSlice*> visibleSlices` and `std::unordered_set<SliceTimeRange>`.
* **Memory accessed:** Only metadata (`startTime_ms`, `endTime_ms`) plus each slice passed into `createCellsFromLiquiditySlice`. The `unordered_set` operations read/write 16-byte keys.
* **Read/write pattern:** Append-mode loops are read-mostly; writes only occur when a new slice appears. Processed ranges remain in the hash set until viewport changes, so repeated calls typically short-circuit.

## 2. Slice → Cells Expansion

```cpp
// libs/gui/render/DataProcessor.cpp:564-583
for (size_t i = 0; i < slice.bidMetrics.size(); ++i) {
    const auto& metrics = slice.bidMetrics[i];
    if (metrics.snapshotCount > 0) {
        double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        if (price >= minPrice && price <= maxPrice) {
            createLiquidityCell(slice, price, slice.getDisplayValue(price, true, 0), true);
        }
    }
}
for (size_t i = 0; i < slice.askMetrics.size(); ++i) {
    const auto& metrics = slice.askMetrics[i];
    if (metrics.snapshotCount > 0) {
        double price = slice.minTick * slice.tickSize + (static_cast<double>(i) * slice.tickSize);
        if (price >= minPrice && price <= maxPrice) {
            createLiquidityCell(slice, price, slice.getDisplayValue(price, false, 0), false);
        }
    }
}
```

* **Iteration count:** Each loop scans contiguous `std::vector<PriceLevelMetrics>` buffers created by the LTSE. With `m_depthLimit = 2000` per side (`LiquidityTimeSeriesEngine.h:160`), there can be up to 2000 iterations per side per slice.
* **Access pattern:** Sequential read-only; each iteration touches `metrics.snapshotCount` and, when passing filters, calls `LiquidityTimeSlice::getDisplayValue`, which performs another O(1) vector lookup by tick.
* **Writes:** `createLiquidityCell` pushes onto `m_visibleCells`. That function writes ~72 bytes per cell plus updates for `m_visibleCells` bookkeeping (size/capacity).

## 3. Dense Book Band Conversion

```cpp
// libs/gui/render/DataProcessor.cpp:337-361
if (!denseBids.empty()) {
    size_t maxIndex = static_cast<size_t>(std::min<double>(denseBids.size(), std::ceil((bandMaxPrice - liveBook.getMinPrice()) / liveBook.getTickSize()) + 1));
    size_t minIndex = static_cast<size_t>(std::max<double>(0.0, std::floor((bandMinPrice - liveBook.getMinPrice()) / liveBook.getTickSize())));
    for (size_t i = maxIndex; i > minIndex && i > 0; --i) {
        size_t idx = i - 1;
        double qty = denseBids[idx];
        if (qty > 0.0) {
            double price = liveBook.getMinPrice() + (static_cast<double>(idx) * liveBook.getTickSize());
            sparseBook.bids.push_back({price, qty});
        }
    }
}
// asks loop (ascending) mirrors this logic
```

* **Iteration count:** The band window typically spans a few hundred ticks around the mid-price, so these loops walk ~500 indices per call even though the dense arrays may be thousands of entries long.
* **Access pattern:** Backward scan for bids, forward scan for asks, reading sequential `double` entries from `std::vector<double>` (SOA). Only non-zero levels trigger writes to the sparse vector (`sparseBook.bids/asks`).
* **Writes:** Each surviving level appends a `{double,double}` struct to the sparse vectors, incurring heap growth when the vector exceeds its reserved capacity.

## 4. Cell Geometry Generation

```cpp
// libs/gui/render/strategies/HeatmapStrategy.cpp:64-133
while (producedKeptCells < keptCells) {
    std::vector<const CellInstance*> chunkCells;
    chunkCells.reserve(targetCells);
    for (; streamPos < cellCount && static_cast<int>(chunkCells.size()) < targetCells; ++streamPos) {
        const auto& c = batch.cells[startIndex + streamPos];
        if (c.liquidity >= batch.minVolumeFilter) {
            chunkCells.push_back(&c);
        }
    }
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    for (const CellInstance* cellPtr : chunkCells) {
        const auto& cell = *cellPtr;
        double scaledIntensity = calculateIntensity(cell.liquidity, batch.intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        QPointF topLeft = CoordinateSystem::worldToScreen(cell.timeStart_ms, cell.priceMax, batch.viewport);
        QPointF bottomRight = CoordinateSystem::worldToScreen(cell.timeEnd_ms, cell.priceMin, batch.viewport);
        vertices[vertexIndex++].set(left,  top,    r, g, b, a);
        // ... total of 6 vertex writes per cell
    }
}
```

* **Iteration count:** Each frame handles all visible cells after filtering. Logs show 20 k–30 k cells, so the vertex loop executes that many times, emitting 6 vertices per cell (120 k–180 k vertex struct writes). `cellsPerChunk` caps each chunk at 10 000 cells (60 000 vertices) to stay within ANGLE 16-bit index limits.
* **Memory accessed:** `batch.cells` is contiguous; the loop touches each `CellInstance` field. Two calls to `CoordinateSystem::worldToScreen` per cell read `Viewport` and simple scalars. Writing into `QSGGeometry` hits a fresh heap buffer (per chunk) sequentially.
* **Read/write pattern:** Strictly sequential and write-only for the vertex buffer; no random access.

## 5. Vertex Chunk Logging

```cpp
// libs/gui/render/strategies/HeatmapStrategy.cpp:142-147
if ((++frame % 30) == 0) {
    sLog_RenderN(1, " HEATMAP CHUNKS: cells=" << keptCells
                         << " verts=" << totalVerticesDrawn
                         << " chunks=" << root->childCount());
}
```

This log couples the runtime counts above to the `HeatmapStrategy` execution path, making it easy to correlate `keptCells` with the vector size used in the loops. When `verts` approaches 200 k, the loop above already emitted that many vertices into Qt’s scene graph for upload.
