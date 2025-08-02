# Sentinel Refactor Plan (v0.1)

---

## 🎯 Objectives

* **Explode the God‑classes.** Break *UnifiedGridRenderer* (\~1.5 kLOC) and *LiquidityTimeSeriesEngine* (\~550 LOC) into composable, test‑friendly units.
* **Stabilize public surfaces** before we hand it off to Claude Code for mechanical refactor passes.
* **Keep the FPS gate ≥ 59\@4K** and memory budget unchanged while we carve things up.

---

## 📊 Baseline Metrics (pre‑refactor)

| File                          | LOC   | Responsibility                          |
| ----------------------------- | ----- | --------------------------------------- |
| UnifiedGridRenderer.cpp       | 1 576 | Rendering, viewport math, data plumbing |
| UnifiedGridRenderer.h         | 327   | Public API, state, Qt props             |
| LiquidityTimeSeriesEngine.cpp | 403   | Snapshot aggregation, anti‑spoof logic  |
| LiquidityTimeSeriesEngine.h   | 162   | Data structures, interface              |
| CoordinateSystem.\*           | 115   | World↔Screen transforms                 |

*Total libs/ footprint: **6 997 LOC / \~64 k tokens**.*

---

## 🗂️ Proposed Top‑Level Layout

```text
core/          # WebSocket, caches, guards
pipeline/      # Ingest → dist → slice → metrics
    collector/          # SnapshotCollector (+ tests)
    aggregator/         # TimeframeAggregator (+ tests)
render/
    scene/              # Qt SG nodes & materials
    strategies/         # HeatmapMode, TradeFlowMode, VolumeCandlesMode
ui/            # QML & widget glue
bridge/        # GridIntegrationAdapter (legacy ↔ new)
tests/
```

---

## 🧩 Module Decomposition

### 1. UnifiedGridRenderer ➡️ `render/`

| Concern                   | New Class                                                                                     | Notes                                                          |
| ------------------------- | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------- |
| Scene‑graph orchestration | `GridSceneNode`                                                                               | Owns the `QSGTransformNode` root — no business logic.          |
| Rendering strategy        | `IRenderStrategy` + concrete modes (`HeatmapStrategy`, `TradeFlowStrategy`, `CandleStrategy`) | Classic Strategy pattern; each returns a populated `QSGNode*`. |
| View state & gestures     | `GridViewState`                                                                               | Holds viewport, zoom, pan; emits signals.                      |
| Debug / Perf overlay      | `RenderDiagnostics`                                                                           | Pulls from shared `PerformanceMonitor`.                        |

### 2. LiquidityTimeSeriesEngine ➡️ `pipeline/`

| Concern                 | New Unit              | Notes                                                   |
| ----------------------- | --------------------- | ------------------------------------------------------- |
| Raw snapshot intake     | `SnapshotCollector`   | Depth‑limited, viewport‑aware, 100 ms timer lives here. |
| Time‑bucket aggregation | `TimeframeAggregator` | Produces immutable `LiquiditySlice` objects.            |
| Metrics & anti‑spoof    | `PersistenceAnalyzer` | Calculates `persistenceRatio`, etc.                     |

---

## 🔌 Shared Interfaces (header‑only)

```cpp
struct Trade { /* existing */ };
struct OrderBook { /* existing */ };

class IOrderFlowSink {
  virtual void onTrade(const Trade&) = 0;
  virtual void onOrderBook(const OrderBook&) = 0;
};

class IRenderStrategy {
  virtual QSGNode* buildNode(const GridSliceBatch& batch,
                             const GridViewState& view) = 0;
  virtual ~IRenderStrategy() = default;
};
```

These seams let us unit‑test aggregation without Qt and swap render modes at runtime.

---

## 🚦 Migration Roadmap & Cursor Prompts

| Phase | Goal                                                        | Cursor Agent Prompt                                                                                    |
| ----- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| 0     | Safety‑net tests on current code                            | `//agent:tests scope=libs goal="Snapshot baseline perf & unit tests"`                                  |
| 1     | Stub interfaces & move logic wholesale (no behavior change) | `//agent:refactor scope=libs/gui/UnifiedGridRenderer.* goal="Extract IRenderStrategy & GridViewState"` |
| 2     | Swap runtime to modular pieces behind feature flag          | `//agent:structure scope=libs render view="call graph"`                                                |
| 3     | Delete legacy paths, tighten perf                           | `//agent:cleanup scope=libs goal="Remove deprecated members & macros"`                                 |

---

## ✅ Acceptance Criteria

* 60 FPS @4 K with 20 k msg/s fire‑hose.
* Memory delta ≤ +3 MB vs baseline.
* Unit coverage ≥ 70 % on pipeline & renderer strategies.
* Debug overlay toggles via `GridSceneNode::setDiagnosticsVisible(true)`.

---

## 🛡️ Adapter Decision (2025‑08‑01)

We’ll **keep `UnifiedGridRenderer` as a thin QML‑facing adapter** for the foreseeable future. The class now:

* Retains all `Q_PROPERTY` and signal surface expected by existing QML.
* Delegates *all* logic to V2 components (`GridViewState`, `GridSceneNode`, strategies).
* Holds **no** viewport state of its own.

Future deprecation path remains open: once V2 is battle‑tested we can mark `UnifiedGridRenderer` as legacy and expose `GridViewState` + `GridSceneNode` directly to QML.

---

## 🔜 Next Actions

1. Run the **V1‑Purge** agent prompt (see chat).
2. Validate mouse pan/drag in the demo scene.
3. If pan still fails, instrument `ViewState::handlePanMove()` with log lines and dump the coordinate deltas for a single drag so we can pinpoint the math bug.


---

**We’re fully synced.** Here’s the quick-hit checklist before you unleash the V1-Purge agent:

| ✅ / 🔲 | Step                                                                                                                                                           | Why it matters |
| ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| ✅      | Docs updated in the canvas: `UnifiedGridRenderer` stays as a **thin QML adapter** (no internal state, pure delegation).                                        |                |
| 🔲     | Confirm no stray QML bits still read `m_visibleTimeStart_ms`, `m_isDragging`, etc. (grep the QML layer).                                                       |                |
| 🔲     | Copy-paste the **V1-Purge prompt** exactly—Cursor will remove the legacy methods, undo the `#ifdef` flag, and rebuild.                                         |                |
| 🔲     | After compile, do a one-drag test: watch the debug log from `ViewState::handlePanMove()`—you should see consistent `dx/dy` values from press → move → release. |                |
| 🔲     | If pan still whiffs, dump the drag deltas and we’ll triangulate the bad math (likely the item-bounds ↔ data-coords conversion).                                |                |

### Quick sanity script (optional)

```bash
# From repo root
rg -n "m_visibleTimeStart_ms|m_isDragging|createHeatmapNode" ui/ qml/
```

Nothing should pop after the purge.

### Next agent after purge

If the click-drag still misbehaves, feed Cursor something like:

```
📌 Context
  • Drag deltas logged: start (-120, 0) → move (-118, 0) → release (-118, 0)
  • View doesn’t move; expect ~2-cell pan.

🎯 Task
  1. Inspect ViewState::handlePanMove() math against GridSceneNode bounds.
  2. Fix incorrect pixel→price/time conversion.
  3. Re-run unit & UI tests.
```

Fire when ready, commander. 🫡
