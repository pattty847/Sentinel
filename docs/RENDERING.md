## Sentinel Rendering Pipeline Overview

### 1. Primary Role of Each Class

| Class | Layer | Role |
|-------|-------|------|
| **`HeatmapTwapStreamer`** | Core (server) | Samples the live order book, accumulates TWAP per price row, encodes dense `u8` columns (bids 0–127, asks 128–255) |
| **`SentinelStreamServer`** | Core (server) | Broadcasts `heatmap_slice` messages (base64 column + metadata) over WebSocket |
| **`RemoteGridDataSource`** | GUI (client) | Receives `heatmap_slice` and emits `heatmapSliceReceived` to the render pipeline |
| **`DataProcessor`** | GUI (worker thread) | For remote heatmaps: validates incoming slices and emits `heatmapColumnReady` (no LTSE aggregation) |
| **`UnifiedGridRenderer`** | GUI (QQuickItem) | Manages viewport state, ring-buffer uploads, and drives `updatePaintNode()` |
| **`HeatmapIntensityNode`** | Render Thread (QSG) | Single-quad material; samples intensity + palette and renders on GPU |

---

### 2. Connections Between Components (Remote Heatmap)

```
Server:
  MarketDataCore → DataCache → LiveOrderBook
      → HeatmapTwapStreamer (TWAP, dense u8 column)
      → SentinelStreamServer (heatmap_slice)

Client:
  RemoteGridDataSource (WebSocket)
      → DataProcessor::onHeatmapSliceReceived
      → UnifiedGridRenderer (ring-buffer enqueue)
      → HeatmapIntensityNode (single quad)
      → GPU
```

---

### 3. Key Data Structures (Remote Heatmap)

```
LiveOrderBook (server)
  → TWAP buckets per row
  → Dense column (u8):
       0–127   bids
       128–255 asks

heatmap_slice (client)
  • time_start / time_end
  • tick_size / min_price / max_price
  • column (base64)
```

---

### 4. Summary Flow (Text)

```
Server:
  LiveOrderBook → HeatmapTwapStreamer → heatmap_slice (u8 column)

Client:
  RemoteGridDataSource → DataProcessor → UnifiedGridRenderer
      → HeatmapIntensityNode (palette LUT) → GPU
```

---

### Notes / Legacy Pipeline

- The old LTSE → CellInstance → GridSceneNode path still exists for non-heatmap layers.
- Remote heatmap rendering must not allow a second local column producer.
