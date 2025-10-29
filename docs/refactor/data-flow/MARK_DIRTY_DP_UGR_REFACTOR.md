### Executive findings
- Full rebuilds are triggered far too often (data updates and user interactions), causing stutter and column “loss.”
- The heatmap is currently rebuilt from scratch on most updates. Incremental rendering is not implemented.
- Geometry is derived from screenRect in the data layer; world-space-first design is not used.
- Startup viewport-size sync is fragile; DP can start before item size is valid.
- “Auto-follow” is not a follower, just a rare recovery path.
- Dense ingestion path is actually active (contrary to the plan’s “dead code” note).

### Audit of FIX_ALOT.md vs current code

#### Part 1: UnifiedGridRenderer – “Four dirty flags”
- Status: Missing. Only `m_geometryDirty` exists; no separate flags.
```85:93:libs/gui/UnifiedGridRenderer.h
std::atomic<bool> m_geometryDirty{true};
std::vector<CellInstance> m_visibleCells;
std::shared_ptr<const std::vector<CellInstance>> m_publishedCells;
```
- Data update triggers full rebuilds:
```327:333:libs/gui/UnifiedGridRenderer.cpp
connect(m_dataProcessor.get(), &DataProcessor::dataUpdated, 
        this, [this]() { 
            m_geometryDirty.store(true);
            update();
        }, Qt::QueuedConnection);
```
- User interactions also mark geometry dirty (shouldn’t):
```240:246:libs/gui/UnifiedGridRenderer.cpp
zoomIn/zoomOut/panLeft/Right/Up/Down → m_geometryDirty.store(true)
```
- updatePaintNode holds a mutex and rebuilds on dirty:
```415:456:libs/gui/UnifiedGridRenderer.cpp
std::lock_guard<std::mutex> lock(m_dataMutex);
bool needsRebuild = m_geometryDirty.load() || isNewNode;
...
sceneNode->updateContent(batch, strategy);
m_geometryDirty.store(false);
```
- Verdict:
  - Accurate recommendation to split: geometryDirty, appendPending, transformDirty, materialDirty.
  - Removing the blanket dirtying is necessary.
  - Mutex in updatePaintNode should be eliminated with atomic handoff.

Improvements:
- Gate DP start until non-zero `geometryChanged` and set viewport size in `componentComplete()` to prevent left-shift at startup.
- While dragging, skip rebuilds (transform-only path).

#### Part 2: DataProcessor – “Append, ring buffer, atomics”
- Status: Missing/incomplete. Uses full clear+rebuild, multiple mutexes, publishes full snapshots.
```350:357:libs/gui/render/DataProcessor.cpp
void DataProcessor::updateVisibleCells() {
    m_visibleCells.clear();  // full destroy
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
```
- Rebuilds all visible slices every update:
```381:436:libs/gui/render/DataProcessor.cpp
auto visibleSlices = m_liquidityEngine->getVisibleSlices(...);
for (const auto* slice : visibleSlices) { createCellsFromLiquiditySlice(*slice); }
```
- Publishes entire vector each time (copy into shared_ptr), then emits `dataUpdated()`:
```445:451:libs/gui/render/DataProcessor.cpp
{
    std::lock_guard<std::mutex> snapLock(m_snapshotMutex);
    m_publishedCells = std::make_shared<std::vector<CellInstance>>(m_visibleCells);
}
emit dataUpdated();
```
- Mutexes still in headers:
```108:119:libs/gui/render/DataProcessor.hpp
mutable std::mutex m_dataMutex;
mutable std::mutex m_snapshotMutex;
std::shared_ptr<const std::vector<CellInstance>> m_publishedCells;
```
- Dense ingestion is enabled (plan says remove as dead; this is incorrect):
```128:130:libs/gui/render/DataProcessor.hpp
bool m_useDenseIngestion = true;
```
```223:240:libs/gui/render/DataProcessor.cpp
if (m_useDenseIngestion) {
    ...
    m_liquidityEngine->addDenseSnapshot(view);
    ...
    updateVisibleCells();
    return;
}
```
- `captureOrderBookSnapshot` uses a static `lastBucket`:
```126:139:libs/gui/render/DataProcessor.cpp
static qint64 lastBucket = 0;  // static state
...
if (bucketStart > lastBucket) { ... updateVisibleCells(); }
```
- Verdict:
  - Accurate recommendation to implement append-only with ring buffer, and snapshot handoff via atomics (or lock-free handoff).
  - The “dead dense ingestion” claim is wrong; it’s actually enabled by default.
  - Static in `captureOrderBookSnapshot` should be moved to a member.

Improvements:
- For atomics on shared_ptr, use std::atomic_load/store free functions if compiler doesn’t support `std::atomic<std::shared_ptr<T>>` well.
- Emit throttling at ~60 Hz max; coalesce bursts; skip emission during drag.

#### Part 3: CellInstance world coordinates-only
- Status: Missing. Data layer stores screenRect; strategies rely on it.
```7:16:libs/gui/render/GridTypes.hpp
struct CellInstance {
    QRectF screenRect;   // screen coords in data layer
    QColor color;
    double intensity;
    double liquidity;
    bool isBid;
    int64_t timeSlot;
    double priceLevel;
};
```
```66:70:libs/gui/render/strategies/HeatmapStrategy.cpp
float left = cell.screenRect.left();
float right = cell.screenRect.right();
float top = cell.screenRect.top();
float bottom = cell.screenRect.bottom();
```
- Verdict:
  - Accurate recommendation to move to world coords and compute screen mapping in the renderer (via `CoordinateSystem` or GPU transform).
  - This is a bigger change; should be staged after dirty-flag fixes.

Improvements:
- In the near-term, even if you keep screenRect, implement append/remove and persistent geometry to avoid full rebuilds.
- Long-term, switch to world→screen via transform (DirtyMatrix for pan/follow; rebuild only on LOD).

Double-panning bug:
- You apply pan both via node transform and inside `worldToScreen()`:
```445:451:libs/gui/UnifiedGridRenderer.cpp
QPointF pan = m_viewState->getPanVisualOffset();
transform.translate(pan.x(), pan.y());
```
```272:276:libs/gui/UnifiedGridRenderer.cpp
QPointF panOffset = getPanVisualOffset();
screenPos.setX(screenPos.x() + panOffset.x());
screenPos.setY(screenPos.y() + panOffset.y());
```
- Fix: remove pan addition in `worldToScreen()`; keep transform at node level.

#### Part 4: Additional fixes (destructor, viewport size, locks)
- Destructor improvement recommended: good idea to `stopProcessing()` via blocking queued call before quitting thread. Current code only quits the thread.
```54:60:libs/gui/UnifiedGridRenderer.cpp
if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) { m_dataProcessorThread->quit(); ... }
```
- Viewport size set in mouse events: exists and should be removed:
```300:306:libs/gui/UnifiedGridRenderer.cpp
m_viewState->setViewportSize(width(), height());  // in mousePress
```
```491:491:libs/gui/UnifiedGridRenderer.cpp
m_viewState->setViewportSize(width(), height());  // in mouseRelease
```
- Mutex in updatePaintNode: exists; should be removed by using atomic pointer handoff and per-flag logic:
```415:456:libs/gui/UnifiedGridRenderer.cpp
std::lock_guard<std::mutex> lock(m_dataMutex);
```

### Clarifications and corrections to FIX_ALOT.md
- Incorrect: “Dense ingestion code is dead.” It’s enabled (`m_useDenseIngestion = true`) and active. Don’t delete; instead, ensure both dense and sparse paths publish deltas for append-only rendering.
- Risky/vague: “Remove mutexes.” Do it only where data structures are immutable or single-producer/single-consumer with atomic pointer swaps. Keep a small lock if you update multiple fields with invariants.
- Vague: “Only rebuild on zoom.” Specify LOD thresholds tied to pixels-per-time and pixels-per-price so you can decide rebuild vs transform deterministically.
- Redundant: Repeating the same 4-flag examples across methods. Replace with a table mapping event → flag:
  - transformDirty: pan, small zoom steps, auto-follow nudge
  - materialDirty: intensity/palette changes
  - appendPending: new slices
  - geometryDirty: render mode change, timeframe or price-resolution change, product switch, major zoom crossing LOD threshold

### Recommended changes (precise, minimal-first)
- UnifiedGridRenderer
  - Add four flags; rewire all setters/handlers to set the right flag.
  - In `initializeV2Architecture`, change `dataUpdated` connection to set `appendPending` not `geometryDirty`.
  - Start DP after first non-zero `geometryChanged`; also set size in `componentComplete()`.
  - Remove `setViewportSize(...)` from mouse events.
  - Remove mutex usage in `updatePaintNode`; branch by flags and use `exchange(false)` semantics.
  - Fix double-panning by removing pan from `worldToScreen`.

- DataProcessor
  - Change `updateVisibleCells()` to append-only:
    - Track `m_lastProcessedTime`; query only new slices; append cells for those slices.
    - Implement a ring buffer for cells to bound memory and avoid churn.
    - Throttle emits to ~60 Hz; coalesce bursts; skip during drag (optionally).
  - Move `lastBucket` to a member; ensure snapshots happen once per tick (no backfill loop).
  - Keep dense ingestion if you rely on it; just feed deltas to renderer.

- Strategy/Scene node
  - Make strategy persistent: keep a QSGGeometry with capacity for N columns; append vertices for new slices; retire old columns.
  - Mark only DirtyGeometry for updated ranges, DirtyMatrix for transform changes.
  - Defer full rebuilds to LOD changes and mode switches.

### Optional: rewrite of vague sections (ready-to-paste into FIX_ALOT.md)
- Replace “Remove mutexes” with:
  - Use atomic pointer handoff for published snapshots:
    - Producer: `atomic_store(&m_publishedCells, make_shared(...))`
    - Consumer: `auto snap = atomic_load(&m_publishedCells);`
  - Keep a small mutex for multi-field updates that must be consistent.
- Add LOD policy text:
  - Rebuild geometry when:
    - pixelsPerTime crosses {2 px/slot, 4 px/slot, 8 px/slot} thresholds
    - pixelsPerPrice crosses bucket thresholds {0.25, 0.5, 1, 5, 10}
  - Otherwise, treat zoom as transform-only.

### Checklist (actionable)
- UnifiedGridRenderer
  - [ ] Add flags m_geometryDirty, m_appendPending, m_transformDirty, m_materialDirty
  - [ ] Change `dataUpdated` connection to set `m_appendPending`
  - [ ] Remove `setViewportSize(...)` from mousePress/Release
  - [ ] Start DP after first non-zero size; set size in componentComplete
  - [ ] Remove `m_dataMutex` lock in `updatePaintNode`; implement 4-flag branching
  - [ ] Remove pan offset from `worldToScreen` to fix double-panning

- DataProcessor
  - [ ] Implement append-only `updateVisibleCells()`; track `m_lastProcessedTime`
  - [ ] Add ring buffer cleanup to cap memory
  - [ ] Throttle `dataUpdated` to ~60 Hz; coalesce
  - [ ] Move `lastBucket` to member; no backfill burst
  - [ ] Keep dense ingestion but publish deltas

- Rendering
  - [ ] Make `HeatmapStrategy` persistent; append/remove columns, don’t allocate every frame
  - [ ] Compute screen coords in renderer from world coords (phase 2)
  - [ ] Use `QSGNode::DirtyMatrix` for pan/follow and `DirtyGeometry` only for touched ranges

### Concrete examples to keep you grounded

- Over-dirtying on data update:
```327:333:libs/gui/UnifiedGridRenderer.cpp
m_geometryDirty.store(true);  // replace with m_appendPending.store(true)
```

- Full clear+rebuild (to eliminate):
```350:357:libs/gui/render/DataProcessor.cpp
m_visibleCells.clear();  // append-only instead
```

- Relying on screenRect in the data layer:
```7:16:libs/gui/render/GridTypes.hpp
QRectF screenRect;  // move to world coords; compute screen in renderer
```

- Double-panning risk:
```272:276:libs/gui/UnifiedGridRenderer.cpp
screenPos += panOffset;  // remove; keep transform at node level
```
```445:451:libs/gui/UnifiedGridRenderer.cpp
transform.translate(pan.x(), pan.y());  // keep
```

### Phase plan (brief)
- Phase 1: Dirty flags, event rewiring, startup gating, remove pan double-apply, skip rebuild during drag.
- Phase 2: Append-only DP + emit throttling; persistent geometry append/remove; coalesce data updates.
- Phase 3: World-space cells + renderer-side transforms; LOD thresholds and rebuild policy.
- Phase 4: Performance polish: profiling, cache stats, long-run memory checks.

If you want, I can turn this into a new doc `docs/refactor/data-flow/FIX_ALOT_AUDIT.md` with the checklist and precise code pointers.

- I reviewed all the relevant files and verified each FIX_ALOT point. If you want me to generate the cleaned-up audit doc as a file, I can add it so you can iterate on it alongside the code.