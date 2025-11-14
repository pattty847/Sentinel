# Sentinel Data Pipeline Structures

All line references use 1-based numbering inside this workspace.

## CellInstance and Visible Cell Storage

```cpp
// libs/gui/render/GridTypes.hpp:11-26
struct CellInstance {
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;
    double liquidity = 0.0;
    bool isBid = true;
    double intensity = 0.0;
    QColor color;
    int snapshotCount = 0;
};
```

* **Layout & size:** Array-of-structs. On LP64 the two `int64_t` values occupy 16 B, `priceMin/priceMax/liquidity/intensity` add 32 B, `bool isBid` is padded to 8 B for alignment, `QColor` stores four 32-bit channels (16 B), and `snapshotCount` (4 B) rounds the struct to ~72 B including padding.
* **Storage:** `std::vector<CellInstance> m_visibleCells` in `DataProcessor` (`libs/gui/render/DataProcessor.hpp:132`). Cells are appended sequentially from `createLiquidityCell` (`libs/gui/render/DataProcessor.cpp:587-616`) while processing a slice, then swapped into a `std::shared_ptr<const std::vector<CellInstance>>` for the renderer (`DataProcessor.hpp:93-96`).
* **Cardinality:** The instrumentation at `libs/gui/render/DataProcessor.cpp:524-529` logs `TotalCells`, which routinely lands in the 20–30 k range. At ~72 B each, `m_visibleCells` consumes 1.4–2.2 MB per batch before duplication into the published snapshot.

## GridSliceBatch Hand-Off

```cpp
// libs/gui/render/GridTypes.hpp:28-35
struct GridSliceBatch {
    std::vector<CellInstance> cells;
    std::vector<Trade> recentTrades;
    double intensityScale = 1.0;
    double minVolumeFilter = 0.0;
    int maxCells = 100000;
    Viewport viewport;
};
```

* **Layout:** Value type copied every time `UnifiedGridRenderer::updatePaintNode` builds a batch (`libs/gui/UnifiedGridRenderer.cpp:502-528`). Cells and trades vectors keep their own heap storage; the struct itself is ~72 B (two vectors + three scalars + `Viewport`).
* **Usage:** Provides the renderer with both the AoS cell buffer and the screen-space transform information required by render strategies. `maxCells`, `minVolumeFilter`, and `intensityScale` act as tunables for culling and shading.

## Order Book DTOs

```cpp
// libs/core/marketdata/model/TradeData.h:78-150
struct OrderBookLevel { double price; double size; };
struct OrderBook {
    std::string product_id;
    std::chrono::system_clock::time_point timestamp;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
};
struct BookDelta { uint32_t idx; float qty; bool isBid; };
class LiveOrderBook {
    std::vector<double> m_bids;
    std::vector<double> m_asks;
    double m_min_price, m_max_price, m_tick_size;
    // ...
};
```

* **Layouts:** Sparse `OrderBook` snapshots are vectors of `{double,double}` pairs (16 B each) owned on the heap. `LiveOrderBook` is dense: two `std::vector<double>` buffers sized to `(max_price-min_price)/tick`, giving struct-of-arrays semantics for fast index access. `BookDelta` is a packed 9-byte record (12 B with padding) used to signal per-level changes.
* **Allocation points:** Sparse snapshots originate on the heap when `DataProcessor::onLiveOrderBookUpdated` builds `OrderBook sparseBook` (`libs/gui/render/DataProcessor.cpp:296-382`). Dense arrays live inside `DataCache`'s `LiveOrderBook` instance and are only referenced (no copies) when `captureDenseNonZero` is called (`TradeData.h:194-206`).

## OrderBookSnapshot

```cpp
// libs/core/LiquidityTimeSeriesEngine.h:37-53
struct OrderBookSnapshot {
    int64_t timestamp_ms;
    std::map<double, double> bids;
    std::map<double, double> asks;
    double getBidLiquidity(double price) const;
    double getAskLiquidity(double price) const;
};
```

* **Layout:** Map-based AOS for sparse price levels (`std::map` nodes heap-allocate key/value/pointers per entry). Each snapshot owns its two trees independently.
* **Storage:** `LiquidityTimeSeriesEngine` keeps a base `std::deque<OrderBookSnapshot> m_snapshots` (header line 136) and pushes every 100 ms capture into it (`LiquidityTimeSeriesEngine.cpp:75-78`).

## LiquidityTimeSlice + PriceLevelMetrics

```cpp
// libs/core/LiquidityTimeSeriesEngine.h:58-120
struct LiquidityTimeSlice {
    int64_t startTime_ms, endTime_ms, duration_ms;
    Tick minTick, maxTick;
    double tickSize;
    struct PriceLevelMetrics {
        double totalLiquidity, avgLiquidity, maxLiquidity, minLiquidity;
        double restingLiquidity;
        int snapshotCount;
        int64_t firstSeen_ms, lastSeen_ms;
        uint32_t lastSeenSeq;
        bool wasConsistent() const;
        double persistenceRatio(int64_t slice_duration_ms) const;
    };
    std::vector<PriceLevelMetrics> bidMetrics;
    std::vector<PriceLevelMetrics> askMetrics;
    double getDisplayValue(double price, bool isBid, int displayMode) const;
};
```

* **Layout:** Each slice is AoS with two dense vectors of `PriceLevelMetrics`. Every metrics struct is 72 B (five doubles, an int, two int64_t, one uint32_t + padding). With thousands of ticks per slice, both `bidMetrics` and `askMetrics` are contiguous SOA-like buffers keyed by `(tick - minTick)`.
* **Storage:** `LiquidityTimeSeriesEngine` holds rolling deques per timeframe (`std::map<int64_t, std::deque<LiquidityTimeSlice>> m_timeSlices`, header line 142) and a single mutable slice per timeframe in `m_currentSlices`. Slices remain owned by the engine; `DataProcessor` only consumes pointers from `getVisibleSlices` (`LiquidityTimeSeriesEngine.cpp:139-162`).

## Liquidity Engine Aggregation State

Key configuration members inside `LiquidityTimeSeriesEngine` (`libs/core/LiquidityTimeSeriesEngine.h:134-162`):

| Member | Purpose | Notes |
| --- | --- | --- |
| `std::vector<int64_t> m_timeframes` | Available aggregation durations | Defaults: 100–10000 ms |
| `size_t m_maxHistorySlices` | Per-timeframe history cap | 5000 slices |
| `double m_priceResolution` | Quantization step for ticks | Shared by snapshot quantization and slice tick math |
| `size_t m_depthLimit` | Max bids/asks per snapshot | 2000 entries per side clamp in `addOrderBookSnapshot` (`LiquidityTimeSeriesEngine.cpp:86-94`) |
| `uint32_t m_globalSequence` | Monotonic stamp for metrics reuse | Stored in `PriceLevelMetrics::lastSeenSeq` |

These members keep slice buffers bounded and aligned with viewport-driven requests from `DataProcessor`.

## HeatmapStrategy Vertex Payload

```cpp
// libs/gui/render/strategies/HeatmapStrategy.hpp:25-29
struct GridVertex {
    float x, y;
    float r, g, b, a;
    float intensity;
};
```

`GridVertex` (28 B) documents the intended vertex payload, although the implementation currently allocates `QSGGeometry::defaultAttributes_ColoredPoint2D` (position + RGBA) and stores intensity only in color channels (`HeatmapStrategy.cpp:95-133`). Geometry is allocated per chunk with `QSGGeometry` owning the heap buffer and freed when the node dies.

## Viewport Snapshot

```cpp
// libs/gui/CoordinateSystem.h:18-25
struct Viewport {
    int64_t timeStart_ms;
    int64_t timeEnd_ms;
    double priceMin;
    double priceMax;
    double width;
    double height;
};
```

`Viewport` is copied into every `GridSliceBatch` (see above). Renderer-side code treats it as immutable world bounds when calling `CoordinateSystem::worldToScreen` (`libs/gui/render/strategies/HeatmapStrategy.cpp:115-120`), so every vertex build touches this 48 B struct.
