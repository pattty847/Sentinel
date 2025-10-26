### Architecture Blueprint

FILE: libs/core/marketdata/footprint/FootprintGrid.hpp
ACTION: CREATE
PURPOSE: Core data model for footprint cells with time circular buffer and price sliding window (bounded memory).
DEPENDENCIES: `libs/core/marketdata/model/TradeData.h` (types), standard library only.
KEY TYPES/METHODS:
- `namespace sentinel::footprint`
- `enum class DeltaMode { Raw, Cumulative, Imbalance, Stacked };`
- `struct FootprintCell { double priceLevel; uint64_t timeStartMs; double bidVol; double askVol; double delta; uint32_t bidCount; uint32_t askCount; float intensity; };`
- `struct FootprintGridConfig { uint64_t bucketSizeMs; size_t maxTimeBuckets; size_t maxPriceLevels; double tickSize; };`
- `class FootprintGrid { public: explicit FootprintGrid(const FootprintGridConfig&); void setTickSize(double); void setViewportPriceRange(double minPrice, double maxPrice); void advanceToBucket(uint64_t bucketStartMs); FootprintCell* mutableCell(uint64_t bucketStartMs, double priceLevel); const FootprintCell* cellAt(uint64_t bucketStartMs, double priceLevel) const; size_t timeBucketCount() const; size_t priceLevelCount() const; uint64_t headBucketStart() const; uint64_t bucketSizeMs() const; double minPrice() const; double maxPrice() const; double tickSize() const; private: /* circular buffer indices and helpers */ };`
INTEGRATION POINTS:
- Owned and used by `libs/core/processors/FootprintProcessor`.
CODE SKELETON:
```cpp
#pragma once
#include <vector>
#include <cstdint>

namespace sentinel::footprint {

enum class DeltaMode { Raw, Cumulative, Imbalance, Stacked };

struct FootprintCell {
    double priceLevel{0.0};
    uint64_t timeStartMs{0};
    double bidVol{0.0};
    double askVol{0.0};
    double delta{0.0};
    uint32_t bidCount{0};
    uint32_t askCount{0};
    float intensity{0.0f};
};

struct FootprintGridConfig {
    uint64_t bucketSizeMs{1000};
    size_t maxTimeBuckets{300};
    size_t maxPriceLevels{256};
    double tickSize{0.01};
};

class FootprintGrid {
public:
    explicit FootprintGrid(const FootprintGridConfig& config);
    void setTickSize(double tick);
    void setViewportPriceRange(double minPrice, double maxPrice);

    void advanceToBucket(uint64_t bucketStartMs);

    FootprintCell* mutableCell(uint64_t bucketStartMs, double priceLevel);
    const FootprintCell* cellAt(uint64_t bucketStartMs, double priceLevel) const;

    size_t timeBucketCount() const;
    size_t priceLevelCount() const;

    uint64_t headBucketStart() const;
    uint64_t bucketSizeMs() const;

    double minPrice() const;
    double maxPrice() const;
    double tickSize() const;

private:
    // ring buffer over time axis
    // sliding window for price axis (minPrice..maxPrice inclusive)
    // helpers: indexOfTime(uint64_t), indexOfPrice(double), clearColumn(size_t)
};

} // namespace sentinel::footprint
```

FILE: libs/core/marketdata/footprint/FootprintGrid.cpp
ACTION: CREATE
PURPOSE: Implement circular time buffer management, price window remapping/clearing, and cell lookup.
DEPENDENCIES: `FootprintGrid.hpp`.
KEY TYPES/METHODS:
- Implement methods declared in header; no Qt.
INTEGRATION POINTS:
- Used by `FootprintProcessor`.
CODE SKELETON:
```cpp
#include "FootprintGrid.hpp"
namespace sentinel::footprint {

// Implement ctor, setTickSize, setViewportPriceRange, advanceToBucket,
// mutableCell, cellAt, and index helpers (without Qt).

} // namespace sentinel::footprint
```

FILE: libs/core/processors/FootprintProcessor.hpp
ACTION: CREATE
PURPOSE: Qt-free processor aggregating trades into `FootprintGrid`; provides read APIs for renderer side.
DEPENDENCIES: `libs/core/marketdata/model/TradeData.h`, `libs/core/marketdata/footprint/FootprintGrid.hpp`, `<mutex>`, `<unordered_map>`.
KEY TYPES/METHODS:
- `namespace sentinel`
- `struct FootprintSlice { const footprint::FootprintCell* cells; size_t timeBuckets; size_t priceLevels; uint64_t startTimeMs; uint64_t bucketSizeMs; double minPrice; double maxPrice; double tickSize; };`
- `class FootprintProcessor { public: explicit FootprintProcessor(); void setBucketSizeMs(uint64_t); void setMaxTimeBuckets(size_t); void setMaxPriceLevels(size_t); void setTickSizeForProduct(const std::string& productId, double tickSize); void onTrade(const Trade&); void onViewportChanged(const std::string& productId, uint64_t startMs, uint64_t endMs, double minPrice, double maxPrice); void setDeltaMode(footprint::DeltaMode); footprint::DeltaMode deltaMode() const; // Data access const FootprintSlice getSlice(const std::string& productId) const; // Utilities uint64_t currentBucketStart(uint64_t tradeMs) const; private: mutable std::mutex m_mutex; std::unordered_map<std::string, footprint::FootprintGrid> m_grids; footprint::DeltaMode m_deltaMode{footprint::DeltaMode::Raw}; uint64_t m_bucketSizeMs{1000}; size_t m_maxTimeBuckets{600}; size_t m_maxPriceLevels{512}; };`
INTEGRATION POINTS:
- Owned by `libs/gui/render/DataProcessor`.
CODE SKELETON:
```cpp
#pragma once
#include <mutex>
#include <unordered_map>
#include <string>
#include "../marketdata/model/TradeData.h"
#include "../marketdata/footprint/FootprintGrid.hpp"

namespace sentinel {

struct FootprintSlice {
    const sentinel::footprint::FootprintCell* cells{nullptr};
    size_t timeBuckets{0};
    size_t priceLevels{0};
    uint64_t startTimeMs{0};
    uint64_t bucketSizeMs{1000};
    double minPrice{0.0};
    double maxPrice{0.0};
    double tickSize{0.01};
};

class FootprintProcessor {
public:
    FootprintProcessor();

    void setBucketSizeMs(uint64_t bucketMs);
    void setMaxTimeBuckets(size_t buckets);
    void setMaxPriceLevels(size_t levels);

    void setTickSizeForProduct(const std::string& productId, double tickSize);

    void onTrade(const Trade& trade);
    void onViewportChanged(const std::string& productId,
                           uint64_t startMs, uint64_t endMs,
                           double minPrice, double maxPrice);

    void setDeltaMode(sentinel::footprint::DeltaMode mode);
    sentinel::footprint::DeltaMode deltaMode() const;

    const FootprintSlice getSlice(const std::string& productId) const;

    uint64_t currentBucketStart(uint64_t tradeMs) const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, sentinel::footprint::FootprintGrid> m_grids;
    sentinel::footprint::DeltaMode m_deltaMode{sentinel::footprint::DeltaMode::Raw};
    uint64_t m_bucketSizeMs{1000};
    size_t m_maxTimeBuckets{600};
    size_t m_maxPriceLevels{512};
};

} // namespace sentinel
```

FILE: libs/core/processors/FootprintProcessor.cpp
ACTION: CREATE
PURPOSE: Implement trade aggregation into buckets and price levels; maintain bounded memory; no Qt.
DEPENDENCIES: `FootprintProcessor.hpp`.
KEY TYPES/METHODS:
- Implement `onTrade`, `onViewportChanged`, `getSlice`, `currentBucketStart`, and grid management.
INTEGRATION POINTS:
- Called by `render/DataProcessor` on trade and viewport changes.
CODE SKELETON:
```cpp
#include "FootprintProcessor.hpp"

namespace sentinel {

FootprintProcessor::FootprintProcessor() = default;

// Implement setters, onTrade (aggregate into grid), onViewportChanged,
// getSlice (returns read-only view), and bucket computation.

} // namespace sentinel
```

FILE: libs/gui/render/strategies/FootprintStrategy.hpp
ACTION: CREATE
PURPOSE: Render footprint cells as batched quads via Qt Scene Graph (QSG) using DataProcessor-provided slices.
DEPENDENCIES: `libs/gui/render/IRenderStrategy.hpp`, `libs/gui/render/DataProcessor.hpp` (for data access), `<QSGGeometryNode>`, `<QColor>`.
KEY TYPES/METHODS:
- `class FootprintStrategy : public IRenderStrategy { public: explicit FootprintStrategy(class DataProcessor*); QSGNode* buildNode(const struct GridSliceBatch& batch) override; QColor calculateColor(double liquidity, bool isBid, double intensity) const override; const char* getStrategyName() const override; // normalization void setPercentile(double p); void setColorScheme(int scheme); private: DataProcessor* m_dataProcessor; double m_percentile{0.95}; int m_scheme{0}; // helpers: computeNormalizationBoundsFromVisible(), mapDeltaToColor(double); };`
INTEGRATION POINTS:
- Instantiated in `UnifiedGridRenderer`, selected via `RenderMode`.
CODE SKELETON:
```cpp
#pragma once
#include "IRenderStrategy.hpp"

class DataProcessor;

class FootprintStrategy : public IRenderStrategy {
public:
    explicit FootprintStrategy(DataProcessor* dataProcessor);

    QSGNode* buildNode(const struct GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override;

    void setPercentile(double p);
    void setColorScheme(int scheme);

private:
    DataProcessor* m_dataProcessor{nullptr};
    double m_percentile{0.95};
    int m_scheme{0};

    // helpers (compute 95th percentile of |delta| over visible, map to diverging palette)
    // QSG node/build helpers for quad geometry
};
```

FILE: libs/gui/render/strategies/FootprintStrategy.cpp
ACTION: CREATE
PURPOSE: Build `QSGGeometryNode` quads from the visible footprint slice; apply 95th percentile normalization; reuse culling via CoordinateSystem through DataProcessor.
DEPENDENCIES: `FootprintStrategy.hpp`, `DataProcessor.hpp`, Qt Quick scene graph.
KEY TYPES/METHODS:
- Implement `buildNode` to fetch `getVisibleFootprintSlice(...)` from `DataProcessor`, build geometry, and return a single node.
INTEGRATION POINTS:
- `UnifiedGridRenderer::getCurrentStrategy()`.
CODE SKELETON:
```cpp
#include "FootprintStrategy.hpp"
#include "../DataProcessor.hpp"
#include <QSGGeometryNode>
#include <QSGFlatColorMaterial>

FootprintStrategy::FootprintStrategy(DataProcessor* dp) : m_dataProcessor(dp) {}

QSGNode* FootprintStrategy::buildNode(const GridSliceBatch& /*batch*/) {
    // Pull footprint slice from DataProcessor, compute percentile bounds,
    // allocate geometry, fill vertices, return node.
    return new QSGGeometryNode();
}

QColor FootprintStrategy::calculateColor(double /*liquidity*/, bool /*isBid*/, double /*intensity*/) const {
    return QColor(255, 255, 255, 255);
}

const char* FootprintStrategy::getStrategyName() const { return "Footprint"; }

void FootprintStrategy::setPercentile(double p) { m_percentile = p; }
void FootprintStrategy::setColorScheme(int scheme) { m_scheme = scheme; }
```

FILE: libs/gui/render/DataProcessor.hpp
ACTION: MODIFY
PURPOSE: Own `FootprintProcessor` (core), mediate trades and viewport updates, and expose read API for strategies; provide tick size and bucket size controls.
DEPENDENCIES: `libs/core/processors/FootprintProcessor.hpp`, existing `LiquidityTimeSeriesEngine`, `GridViewState`.
KEY TYPES/METHODS:
- Members: `std::unique_ptr<FootprintProcessor> m_footprintProcessor;`
- Public API: `void setFootprintBucketMs(uint64_t); void setFootprintMaxTimeBuckets(size_t); void setFootprintMaxPriceLevels(size_t);`
- Strategy accessors: `struct FootprintView { const sentinel::FootprintSlice slice; }; FootprintView getVisibleFootprint(const std::string& productId) const;`
- Viewport hook: `void onViewportChangedForFootprint();`
- Trade hook: within `onTradeReceived(const Trade&)` → forward to `FootprintProcessor::onTrade`
- Tick size propagation: `void setTickSizeForProduct(const std::string&, double);`
INTEGRATION POINTS:
- Called by `UnifiedGridRenderer` on viewport/timeframe updates; strategies query via `getVisibleFootprint`.
CODE SKELETON:
```cpp
// Inside class DataProcessor
public:
    void setFootprintBucketMs(uint64_t bucketMs);
    void setFootprintMaxTimeBuckets(size_t buckets);
    void setFootprintMaxPriceLevels(size_t levels);
    void setTickSizeForProduct(const std::string& productId, double tickSize);

    struct FootprintView {
        sentinel::FootprintSlice slice;
    };
    FootprintView getVisibleFootprint(const std::string& productId) const;

private:
    std::unique_ptr<sentinel::FootprintProcessor> m_footprintProcessor;
    void onViewportChangedForFootprint();
```

FILE: libs/gui/render/DataProcessor.cpp
ACTION: MODIFY
PURPOSE: Implement ownership and bridging to `FootprintProcessor`: forward trades, handle viewport changes, and expose slice to strategies.
DEPENDENCIES: `FootprintProcessor.hpp`.
KEY TYPES/METHODS:
- In ctor: `m_footprintProcessor = std::make_unique<sentinel::FootprintProcessor>();`
- In `onTradeReceived`: `m_footprintProcessor->onTrade(trade);`
- In viewport changes (existing `updateVisibleCells`/`onViewportChanged` path): call `onViewportChangedForFootprint()`.
- Implement getters/setters declared in header.
INTEGRATION POINTS:
- No Qt types pass into core; conversion stays in GUI.

FILE: libs/gui/UnifiedGridRenderer.h
ACTION: MODIFY
PURPOSE: Introduce Footprint render mode and Footprint strategy pointer; no raw OpenGL.
DEPENDENCIES: `FootprintStrategy.hpp`.
KEY TYPES/METHODS:
- Extend `enum class RenderMode` with `Footprint`.
- Member: `std::unique_ptr<IRenderStrategy> m_footprintStrategy;`
- `IRenderStrategy* getCurrentStrategy() const;` updated to return footprint strategy on `RenderMode::Footprint`.
INTEGRATION POINTS:
- Strategy selection in paint pipeline; nothing else changes.
CODE SKELETON:
```cpp
// enum class RenderMode { ..., Footprint };
private:
    std::unique_ptr<IRenderStrategy> m_footprintStrategy;
// in getCurrentStrategy(): if (m_renderMode == RenderMode::Footprint) return m_footprintStrategy.get();
```

FILE: libs/gui/UnifiedGridRenderer.cpp
ACTION: MODIFY
PURPOSE: Instantiate `FootprintStrategy` with `DataProcessor*` during V2 initialization; ensure redraw when dataUpdated() fires.
DEPENDENCIES: `FootprintStrategy.hpp`, `DataProcessor.hpp`.
KEY TYPES/METHODS:
- In `initializeV2Architecture()`: `m_footprintStrategy = std::make_unique<FootprintStrategy>(m_dataProcessor.get());`
- In `getCurrentStrategy()`: return `m_footprintStrategy.get()` when mode is Footprint.
INTEGRATION POINTS:
- Tie into existing `updatePaintNodeV2` logic; no new VBO code.

FILE: libs/gui/MainWindowGpu.h / libs/gui/MainWindowGpu.cpp
ACTION: MODIFY (minimal, if needed)
PURPOSE: None required for trade wiring (already routed through `StatisticsController`). Optionally expose a UI control to switch `RenderMode` to Footprint later.
DEPENDENCIES: `UnifiedGridRenderer`.
KEY TYPES/METHODS:
- No new wiring now; future: set renderer mode to `Footprint` via signal/slot when user toggles.

FILE: libs/gui/CMakeLists.txt
ACTION: MODIFY
PURPOSE: Add `FootprintStrategy` sources; ensure they are included in `sentinel_gui_lib`.
DEPENDENCIES: New files path.
KEY TYPES/METHODS:
- Append to `GRID_CORE_SOURCES`: `render/strategies/FootprintStrategy.hpp`, `render/strategies/FootprintStrategy.cpp`.
INTEGRATION POINTS:
- Existing target `sentinel_gui_lib`.

FILE: libs/core/CMakeLists.txt
ACTION: MODIFY
PURPOSE: Add footprint core files to `sentinel_core`.
DEPENDENCIES: New files path.
KEY TYPES/METHODS:
- Add `marketdata/footprint/FootprintGrid.hpp`, `marketdata/footprint/FootprintGrid.cpp`, `processors/FootprintProcessor.hpp`, `processors/FootprintProcessor.cpp`.

FILE: tests/marketdata/test_FootprintProcessor.cpp
ACTION: CREATE
PURPOSE: Unit tests for processor aggregation, ring buffer rollover, price window, and delta modes; live-only.
DEPENDENCIES: `gtest`, `FootprintProcessor.hpp`.
KEY TYPES/METHODS:
- Fixtures to push trades across buckets and price levels; assert cell aggregates and wrap-around behavior.
INTEGRATION POINTS:
- Use existing `tests/marketdata` suite (wired in CMake).

### Critical Questions Answered

1) Where exactly does FootprintProcessor get instantiated and owned?
- Owned by `libs/gui/render/DataProcessor` as `std::unique_ptr<FootprintProcessor>`. Constructed in `DataProcessor` ctor and configured (bucket size, max buckets, tick size per product) by `DataProcessor`.

2) How does FootprintStrategy get data from DataProcessor?
- Pull model: `FootprintStrategy::buildNode` calls `DataProcessor::getVisibleFootprint(productId)` to obtain a `FootprintSlice` view for the current viewport/timeframe. No Qt crossing into core.

3) What's the exact data structure passed from processor → strategy?
- `sentinel::FootprintSlice` containing:
  - pointer to contiguous `FootprintCell` array (row-major time×price),
  - counts: `timeBuckets`, `priceLevels`,
  - `startTimeMs`, `bucketSizeMs`, `minPrice`, `maxPrice`, and `tickSize`.

4) How do we handle the circular buffer index wrap-around?
- `FootprintGrid` maintains a head index for time buckets. On each new bucket, it advances head and clears the new column. Index computation maps requested `bucketStartMs` to column via modulo; strategy reads via `startTimeMs + i * bucketSizeMs`.

5) How does viewport zoom level reach FootprintProcessor to adjust bucketing?
- `DataProcessor` observes viewport changes (already has `GridViewState`); on change, it picks an appropriate footprint bucket size (heuristic tied to visible time span and target columns), sets `FootprintProcessor::setBucketSizeMs`, and calls `onViewportChanged(productId, ...)` to adjust price window.

6) What's the thread safety model? (Mutex? Lock-free queue? RCU?)
- Core processor uses a `std::mutex` protecting grid state. Trades arrive on `DataProcessor`’s thread and are forwarded synchronously to `FootprintProcessor`. Strategies read via `getVisibleFootprint` under the same mutex (short lock, copy-as-view). If contention emerges, we’ll evolve to double-buffer snapshots.

7) How do we get tickSize into FootprintProcessor? (Pass on construction? Query per trade?)
- `DataProcessor` retrieves product metadata (tick size) from its existing sources (e.g., `DataCache`/product info) and calls `setTickSizeForProduct(productId, tick)` on `FootprintProcessor`. Fallbacks avoided in core.

### Review Checklist Compliance

- ✓ No Qt types in `libs/core/*` files.
- ✓ `FootprintStrategy` implements `IRenderStrategy` and returns a `QSGNode`.
- ✓ `render/DataProcessor` remains central ownership point for processors/caches.
- ✓ Ring buffer on time axis; sliding window on price axis with bounded `maxPriceLevels`.
- ✓ Percentile-based color normalization (95th percentile of visible deltas) scoped inside `FootprintStrategy`.
- ✓ Clear separation: aggregation in core (`FootprintProcessor`), rendering in GUI (`FootprintStrategy`).

### Notes on Backfill
- Live-only for Phase 1–2. No historical fetcher in core. Any future fetcher lives in `libs/gui/network/` and hydrates `DataProcessor`, not core.

### Next Steps to Start Work
- Create branch `feature/tpo-footprint` from your current branch.
- Implement headers and minimal scaffolding per above, wire CMake, and add unit tests.
- Enable `RenderMode::Footprint` path and verify geometry populates as trades arrive.