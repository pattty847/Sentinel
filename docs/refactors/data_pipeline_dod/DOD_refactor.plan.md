<!-- c99c5ef2-c8e6-4a99-b5c8-b28ca4e278f5 bdc5a0ce-4dcb-4639-b106-c61977050c8f -->
## Sentinel Dataflow & DOD Refactor Plan (Phases 5–9)

### Context & Invariants

- **Completed:** Phases 1–4 of `sentinel-dataflow-migration-plan-3d37b9b4`:
- `GridSliceBatch` removed from code, only historical mentions left in docs.
- All strategies (`Heatmap`, `TradeBubble`, `TradeFlow`, `Candle`) consume data exclusively via `IDataAccessor` / `UGRDataAccessor`.
- Trades come from `DataCache`, cells from `DataProcessor`, per-frame config/viewport from `UGRDataAccessor`.
- **DOD findings (pre-side-mission):**
- `pipeline_flow.md`: documents the **triple-copy** chain worker → snapshot → GUI → render, and per-frame `GridSliceBatch` construction.
- `data_structures.md`: shows **fat `CellInstance` AoS** and other heavy DTOs.
- `hot_paths.md`: details the **cell expansion loops**, `HeatmapStrategy` vertex loop, and QSG geometry churn.
- `current_bottlenecks.md`: correlates `cacheUs` (data fetch/copy + batch build) and `contentUs` (geometry) against frame budget.
- **Non‑negotiable APIs:**
- `IDataAccessor` remains the **only** API surface for strategies.
- Render strategies’ public interfaces stay stable.
- No new direct coupling from strategies to `UnifiedGridRenderer` or `DataProcessor`.

---

## Phase 5: Snapshot + Compact Cell Layout Integration

### Goals

- Replace the legacy copy-heavy snapshot path with a **double-buffered, read-only snapshot model**.
- Introduce a **compact cell buffer layout** (slim AoS or SoA) behind `DataProcessor` and `UGRDataAccessor`.
- Keep `IDataAccessor::getVisibleCells()` as the stable surface while changing the **implementation & backing storage**.
- Eliminate remaining copies in the cell path: worker → snapshot → GUI → render.

### Rationale (from docs)

- **`pipeline_flow.md`**:
- Documents that `DataProcessor` builds `m_visibleCells` (AoS), then copies into a `shared_ptr<vector<CellInstance>>`, then `UnifiedGridRenderer::updateVisibleCells` copies again before (historically) building `GridSliceBatch`.
- Even after removing `GridSliceBatch`, the worker→snapshot→GUI copy pattern remains the main structural inefficiency.
- **`current_bottlenecks.md`**:
- Ties `cacheUs` directly to `updateVisibleCells()` and batch construction, showing that this path is a visible fraction of the frame budget.
- **`data_structures.md`**:
- Shows `CellInstance` as a **large AoS** (originally ~72 B) stored in `std::vector<CellInstance> m_visibleCells` and duplicated into published snapshots.
- **`hot_paths.md`**:
- Confirms that cell expansion in `DataProcessor` plus per-frame copies are a major cost before geometry even starts.

### Tasks

1. **Design the snapshot model**

- Replace “build `m_visibleCells` → copy into snapshot → GUI copy” with:
 - A **double-buffered or ring-buffered snapshot**: two (or N) `shared_ptr` buffers swapped atomically or under a short lock.
 - A clear **ownership model**: worker thread is the **sole writer**, GUI/render threads are read-only consumers.
- Define a small struct or alias (e.g. `CellSnapshotHandle`) encapsulating:
 - Pointer to compact cell buffer.
 - Count and possibly a frame/version stamp for debugging.

2. **Introduce a compact cell layout behind `DataProcessor`**

- Define a **new internal cell representation**, e.g.:
 - Slim AoS: only time range, price range, liquidity, side, minimal flags (types chosen to fit cache lines).
 - Or SoA: parallel arrays for times, prices, liquidity, side flags, etc.
- Keep the old `CellInstance` type only where strictly required (e.g. transitional debug or non-hot paths), and **stop using it on the hot render path**.
- Update `createCellsFromLiquiditySlice` / `createLiquidityCell` to populate this compact layout instead of (or in addition to, behind a flag) the legacy AoS.

3. **Rewire snapshot publication to use the compact layout**

- Change `DataProcessor` snapshot publication to snapshot the new compact layout:
 - Build into the “back” buffer.
 - Publish by swapping a `shared_ptr<const CompactCellBuffer>` (or similar) under `m_snapshotMutex` / atomic index.
- Update `DataProcessor`’s public snapshot getter (currently `getPublishedCellsSnapshot()`) to return a handle/pointer to the compact buffer (while preserving the `shared_ptr` lifetime semantics).

4. **Bridge `UGRDataAccessor::getVisibleCells()` to new snapshots**

- Internally adapt `UGRDataAccessor::getVisibleCells()` so that:
 - It returns a **stable snapshot pointer** into the new compact buffer model.
 - If needed for a transition, wrap the compact representation to present a `vector<CellInstance>`-like view to existing strategies, but aim to **avoid building full AoS copies** unless a feature flag requires it.
- Document clearly in `IDataAccessor.hpp` / `UGRDataAccessor` that:
 - `getVisibleCells()` returns an immutable per-frame snapshot.
 - Lifetime is tied to the underlying `shared_ptr` owned by `DataProcessor`.

5. **Remove GUI-side cell copies**

- Eliminate any remaining `UnifiedGridRenderer::m_visibleCells` copying that exists only for feeding strategies:
 - Keep GUI-local debug counters or cheap metadata if needed, but no re-copy of the full buffer.
- Ensure all strategy consumption paths go through `UGRDataAccessor` snapshot, not a second GUI-owned vector.

6. **Instrumentation & safety**

- Extend existing logging minimally:
 - Log snapshot version / pointer address occasionally to confirm double-buffer behavior.
 - Add optional debug assertions that no writes occur from GUI/render threads into snapshot memory.
- Possibly wrap the new layout in a tiny debug-only “read-only view” that asserts against mutation in debug builds.

### Expected Perf Benefits

- **`cacheUs` reduction**: Removing redundant copies and shrinking the per-frame data footprint significantly cuts CPU time before geometry.
- **Lower memory bandwidth**: Compact layout + no double copying reduces bytes moved per frame.
- **Better cache locality**: New layout can be tuned to fit 2+ cells per cache line vs <1 today, improving both worker-side and render-side traversal.

### Risks & Mitigations

- **Risk: Snapshot lifetime / threading bugs**
- Mitigation: Centralize ownership in `DataProcessor`, use `shared_ptr` + short locked or atomic index swaps; debug-only assertions for thread identity.
- **Risk: Temporary duplication during transition**
- Mitigation: Keep a strict feature flag (e.g. `USE_COMPACT_CELLS`) and remove legacy paths once compact layout is validated.
- **Risk: Strategy expectations about `CellInstance`**
- Mitigation: First present a compatible view over the compact layout; only later, in Phase 7, rewrite `HeatmapStrategy` to target the compact representation directly.

### Deployment / Merge Strategy

- Implement behind a **feature flag** to allow toggling between old and new snapshot behavior.
- Merge once:
- `UGR paint total/cache/content` with the flag on are at least as good as baseline.
- No correctness/visual regressions are observed on BTC-USD under normal load.
- After a soak period, remove the legacy AoS-only path and make the compact snapshot model the default.

---

## Phase 6: Renderer Consumption Rework

### Goals

- Rework renderer-side consumption (UGR + `GridSceneNode` + accessors) to use **compact snapshots directly**, without extra indirection or per-frame allocations.
- Prepare geometry generation for stable node usage and future DOD improvements.
- Preserve `IDataAccessor` API while optimizing everything **behind** it.

### Rationale (from docs)

- **`pipeline_flow.md`**:
- Shows that `updatePaintNode` previously built new `GridSliceBatch` objects per frame; while we removed those, the pattern of “rebuild everything on geometry/append dirty” still exists.
- **`current_bottlenecks.md`**:
- `FULL GEOMETRY REBUILD` and `APPEND PENDING` logs signal heavy frames; renderer-side architecture dictates how often we pay the geom cost.
- **`hot_paths.md`**:
- Details how `HeatmapStrategy` iterates over `batch.cells` and builds `QSGGeometry` per chunk.
- Highlights QSG node churn and vertex buffer allocations per frame.

### Tasks

1. **Tighten UnifiedGridRenderer’s responsibilities**

- Minimize what `UnifiedGridRenderer::updatePaintNode` does before calling strategies:
 - Build `Viewport` and scalar config only.
 - Construct a `UGRDataAccessor` bound to the current compact snapshot + config.
- Ensure it does **no per-cell work** and avoids temporary containers beyond what’s strictly necessary.

2. **Clarify accessor usage in GridSceneNode**

- Make `GridSceneNode::updateLayeredContent` the single orchestrator of strategy calls using `IDataAccessor*`.
- Confirm it does not maintain any hidden caches of cell data; it should only forward accessor + config flags to strategies.

3. **Remove any remaining references to batch semantics**

- Clean out any lingering comments or assumptions about “batches” in renderer code that could mislead future refactors.
- Replace them with language about “immutable frame snapshots” and “accessor-driven data”.

4. **Prepare for stable QSG node usage**

- Within `GridSceneNode`, identify where child `QSGNode`s are destroyed and recreated every frame.
- Sketch (in comments / TODOs) where we can:
 - Reuse `QSGGeometryNode`s per layer.
 - Update geometry buffers in place where Qt’s API allows.
- Actual geometry reuse will be implemented in Phase 7, but **the structure and ownership must be clear here**.

5. **Refine dirty-flag semantics**

- Double-check:
 - Geometry-level dirty flags trigger only when cell data or major config changes (mode/LOD/timeframe) actually require a rebuild.
 - Append/material/transform flags never go back to doing redundant cell fetches/copies.
- Align comments and log messages with the new snapshot model (no talk of batches).

### Expected Perf Benefits

- Lower per-frame **CPU overhead in the render loop** by avoiding redundant work and allocations before hitting strategies.
- Cleaner layering makes subsequent DOD and geometry reuse changes simpler and safer.

### Risks & Mitigations

- **Risk: Overly aggressive dirty-flag pruning can skip needed rebuilds**
- Mitigation: Keep verbose debug logging of dirty-flag decisions in dev builds and validate via viewport/LOD change scenarios.
- **Risk: Hidden batch-era assumptions linger**
- Mitigation: Systematically grep for `batch`/`GridSliceBatch` semantics in renderer/strategy comments and purge or update them.

### Deployment / Merge Strategy

- Small, focused PR:
- No functional changes to strategy logic yet—only how `UGR` and `GridSceneNode` hand data to them.
- Validate:
- `UGR paint cacheUs` unaffected or reduced.
- No visible regressions in layer toggling, timeframe changes, or zoom/pan behavior.

---

## Phase 7: Heatmap DOD Rewrite

### Goals

- Rewrite `HeatmapStrategy` (and any helper code it uses) to consume the **compact cell layout** with true DOD patterns.
- Reduce per-cell work in the render loop to the minimum required: filtering, 2× `worldToScreen`, vertex writes.
- Mitigate **geometry churn** by reusing QSG nodes/geometry where possible.

### Rationale (from docs)

- **`hot_paths.md`**:
- Shows the main furnace: `HeatmapStrategy::buildNode` loops over 20–30k cells, builds `chunkCells` vectors, and writes 6 vertices per cell into newly allocated `QSGGeometry`.
- Each frame can generate 120–180k vertex writes plus per-chunk heap allocations.
- **`data_structures.md`**:
- Highlights `GridVertex` payload and the mismatch with current `QSGGeometry::ColoredPoint2D` usage.
- **`current_bottlenecks.md`**:
- Confirms that `contentUs` scales linearly with `cells` and vertex counts, and that geometry rebuilds dominate when many slices/cells are visible.

### Tasks

1. **Adapt HeatmapStrategy to compact cells**

- Change `HeatmapStrategy::buildNode` to:
 - Read directly from the compact cell buffer returned via `IDataAccessor::getVisibleCells()` (or a new accessor method if we expose a dedicated compact view).
 - Avoid building intermediate `std::vector<const CellInstance*> chunkCells` where possible; prefer simple index-based iteration over the contiguous buffer.

2. **Minimize per-cell computation**

- Where feasible, move:
 - Intensity normalization/clamping into `DataProcessor` when constructing the compact cells.
 - Simple flags (e.g. `isBid`) into bit-packed fields to reduce branchiness and memory loads.
- In the render loop:
 - Only compute what cannot be precomputed (e.g. 2× `worldToScreen` using current viewport).

3. **Align vertex format with needs**

- Re-evaluate `GridVertex` vs `QSGGeometry::ColoredPoint2D`:
 - If intensity is always derivable from color, keep default attributes and avoid extra per-vertex fields.
 - If an explicit intensity is needed, ensure it’s stored efficiently and accessed with minimal overhead.
- Ensure per-vertex writes are strictly sequential and avoid unnecessary temporaries.

4. **Geometry churn mitigation**

- In cooperation with `GridSceneNode`, adjust `HeatmapStrategy` so that:
 - QSG nodes are **reused** across frames where possible (e.g. one heatmap node per layer reused, only vertex buffers updated).
 - Chunking still respects ANGLE/Qt limits (e.g. ≤60k vertices per node), but nodes themselves stick around and recycle geometry.
- Reduce QSG node destruction/creation in `updateLayeredContent`.

5. **Instrumentation & validation**

- Keep `HEATMAP CHUNKS: cells=X verts=Y chunks=Z` logging and extend if necessary to:
 - Capture average vertices per chunk, reuse count, and total geometry allocations.
- Validate that:
 - For the same `cells` count, `contentUs` decreases or becomes more stable.
 - Node count stabilizes instead of fluctuating wildly with each frame.

### Expected Perf Benefits

- Significant reduction in **`contentUs`** for typical 20–30k cell frames.
- Lower geometry allocation overhead and fewer QSG node creations.
- Better GPU feed behavior thanks to predictable, stable vertex buffer sizes and layouts.

### Risks & Mitigations

- **Risk: QSG reuse semantics misunderstood**
- Mitigation: Follow Qt Quick guidelines for `QSGGeometry` reuse; test under resize, layer toggle, and theme changes.
- **Risk: Visual regressions in color/intensity mapping**
- Mitigation: Preserve mapping formulas while relocating computations; add debug overlays or screenshots for A/B comparison.

### Deployment / Merge Strategy

- Gate major geometry reuse changes behind a **dev flag** initially (e.g. `USE_HEATMAP_NODE_REUSE`).
- Compare logs **before vs after**:
- Same `cells`, fewer `chunks` or fewer allocations.
- Lower or equal `contentUs` across representative viewports.
- Merge once visual output and logs match or beat current behavior.

---

## Phase 8: Backend Slice & LTSE Data-Oriented Improvements

### Goals

- Apply DOD principles to the **worker-side pipeline**:
- Slice selection, cell expansion, and interaction with `LiquidityTimeSeriesEngine`.
- Optimize **dense/sparse ingestion** and slice storage to minimize CPU and memory overhead ahead of cell creation.
- Prepare the whole backend to produce GPU-friendly data with minimal wasted work.

### Rationale (from docs)

- **`pipeline_flow.md`**:
- Describes dense ingestion, sparse fallback, and timer-driven snapshots inside `DataProcessor`, plus how LTSE snapshots and slices are managed.
- **`hot_paths.md`**:
- Shows the slice-processing loop (`updateVisibleCells`), including:
 - Iteration over `visibleSlices`.
 - Massive per-slice loops over `bidMetrics` / `askMetrics` (`createCellsFromLiquiditySlice`), up to 2000 ticks per side.
- **`data_structures.md`**:
- Details `OrderBook`, `LiveOrderBook`, `OrderBookSnapshot`, `LiquidityTimeSlice`, and `PriceLevelMetrics` layouts, including their memory sizes and allocation behaviors.
- **`current_bottlenecks.md`**:
- Links LTSE slice counts and `TotalCells` directly to cache and geometry time; indicates that wide viewports and deep order books amplify worker-side cost.

### Tasks

1. **Worker-side pipeline cleanup**

- Remove any leftover dataflow assumptions that were tied to `GridSliceBatch` or GUI specifics.
- Ensure `DataProcessor`’s interface to LTSE is clean, with DOD-friendly slice/query APIs.

2. **Optimize slice selection & expansion**

- For `DataProcessor::updateVisibleCells`:
 - Ensure `m_processedTimeRanges` and `visibleSlices` are used in a cache-friendly manner.
 - Consider storing slice time ranges in a more compact, cache-aware structure.
- For `createCellsFromLiquiditySlice`:
 - Revisit loops over `bidMetrics`/`askMetrics`:
 - Minimize branching and repeated math (e.g. price reconstruction).
 - Possibly precompute or cache per-slice values used across cells.

3. **Dense/sparse ingestion improvements**

- In `onLiveOrderBookUpdated`:
 - Ensure dense band selection loops over `LiveOrderBook` use contiguous buffers optimally (as already hinted by SoA `m_bids`/`m_asks`).
 - Reduce temporary allocations for sparse `OrderBook` where possible (reserve vectors, reuse structures).
- Adjust `BandMode`/band-width heuristics to keep the number of ingested levels reasonable without starving the renderer.

4. **Struct-of-arrays conversions where applicable**

- Evaluate `LiquidityTimeSlice::PriceLevelMetrics` and related structures:
 - Where read patterns are mostly per-field across many metrics, consider SoA or better packing of the hottest fields.
- Avoid unnecessary doubles where floats are adequate, especially for metrics that feed into per-cell `liquidity` or intensity.

5. **Memory layout audits & cache alignment**

- Use `data_structures.md` guidance to:
 - Tune alignment of critical structs to cache line boundaries where it helps.
 - Remove unused or rarely-used fields from hot-path structs, or move them into side caches/debug structures.

6. **SIMD and conversion opportunities**

- Identify spans where:
 - Liquidity normalization, min/max clamps, or simple arithmetic could be vectorized (SIMD) across multiple ticks or slices.
- Eliminate unnecessary double-to-float conversions or vice versa:
 - Standardize on float or double per stage, to reduce casts and maintain numeric stability where needed.

### Expected Perf Benefits

- Lower CPU time in `DataProcessor::updateVisibleCells` and related ingestion paths.
- More predictable and bounded cell counts per frame for a given viewport.
- Better cache utilization across LTSE + DataProcessor, enabling higher-frequency updates or larger viewports without blowing the frame budget.

### Risks & Mitigations

- **Risk: Subtle changes to slice semantics**
- Mitigation: Keep existing unit/integration tests for LTSE behavior where present; add targeted tests for slice timing and coverage if missing.
- **Risk: Over-aggressive pruning of levels/slices**
- Mitigation: Make any more aggressive limits configurable; monitor logs for missing liquidity in edge cases.

### Deployment / Merge Strategy

- Roll out backend optimizations incrementally:
- First in slice selection & expansion.
- Then in ingestion.
- Then in optional SoA/SIMD passes.
- Each change should be validated with:
- `SLICE PROCESSING` logs (slices processed).
- `TotalCells` and resulting `UGR paint cacheUs`.
- Visual inspection of liquidity coverage.

---

## Phase 9: Final Optimization, Benchmarks, & Validation

### Goals

- Validate the **end-to-end DOD pipeline** from ingestion to GPU:
- Confirm that the pipeline is GPU-feed-friendly and meets frame budgets at target cell counts.
- Lock in performance characteristics with repeatable benchmarks and improved instrumentation.
- Clean up flags, legacy paths, and stale docs, leaving a clear, maintainable architecture.

### Rationale (from docs)

- The four DOD docs (`pipeline_flow.md`, `data_structures.md`, `hot_paths.md`, `current_bottlenecks.md`) together define:
- Where time is spent (slice expansion, cell creation, geometry).
- Where bytes move (AoS cell copies, vertex buffers).
- How much headroom we have vs the 16.67 ms frame budget.

### Tasks

1. **Establish benchmark scenarios**

- Define a small set of repeatable scenarios:
 - Typical BTC-USD stream with default viewport.
 - Wide viewport with many slices/cells.
 - Stress cases (max configured depth/timeframes).
- For each scenario, capture:
 - `UGR paint total/cache/content/cells`.
 - `HEATMAP CHUNKS` logs.
 - LTSE slice counts and `TotalCells`.

2. **Finalize flags and code paths**

- Remove transitional flags once the compact snapshot + DOD pipeline is stable.
- Keep only:
 - Essential debug/diagnostic toggles.
 - Feature flags for experimental SIMD or more aggressive backends, if needed.

3. **Add targeted assertions & diagnostics (debug-only)**

- In debug builds:
 - Assert read-only behavior for snapshots.
 - Validate consistent counts between `DataProcessor` logs and renderer logs (cells, slices, vertices).
- Ensure these checks are cheap or compiled out in release.

4. **Documentation & diagrams**

- Update:
 - `docs/refactors/data_pipeline_dod/*` to reflect:
 - New snapshot model.
 - Compact cell layout.
 - Updated hot paths (with new code references).
 - `docs/refactors/dataflow/proposed_architecture.md` to align with the final accessor-based + DOD-optimized design.
- Include “before vs after” sections:
 - Copy counts.
 - Typical cell sizes/layout.
 - Representative `cacheUs` / `contentUs` numbers.

5. **Merge & rollout strategy**

- Land the final DOD work on a dedicated feature branch (e.g. `feature/dod-pipeline`).
- Run through the benchmark scenarios and any internal demo scenarios.
- Only then merge back to `dev` / `main` with a clear summary of:
 - Perf wins.
 - Architectural changes.
 - Any remaining future opportunities.

### Expected Perf Benefits

- Stable performance under target loads with **predictable frame budgets**.
- Clear understanding of performance envelopes and tradeoffs for future features.
- Easier future optimization thanks to clean layering and well-documented hot paths.

### Risks & Mitigations

- **Risk: Benchmarks mask corner cases**
- Mitigation: Include a mix of real-market replays and synthetic extremes; document known limits.
- **Risk: Overfitting to one GPU or platform**
- Mitigation: Capture basic stats on at least two hardware configs (e.g. dev RTX + a more modest GPU) where possible.

### Deployment / Merge Strategy

- Treat Phase 9 as the **stabilization and documentation phase**:
- No major API changes.
- No large-scale rewrites—only tuning, validation, and cleanup.
- Once complete:
- Freeze the DOD design as the new baseline.
- Use future branches for incremental, localized improvements rather than big pipeline overhauls.

### To-dos

- [ ] Verify current post-Phase-4 dataflow (IDataAccessor usage, UGRDataAccessor config ownership, DataProcessor/UGR responsibilities) matches the migration plan and DOD docs.
- [ ] Refactor DataProcessor cell storage and snapshot publication to use a more DOD-friendly layout and eliminate redundant copies while preserving IDataAccessor API and threading guarantees.
- [ ] Simplify UnifiedGridRenderer and UGRDataAccessor so that all frame config and cell access flow cleanly through the accessor with minimal per-frame overhead and clear logging.
- [ ] Apply DOD-driven optimizations to HeatmapStrategy and GridSceneNode consumption of accessor data to reduce per-cell work and geometry churn without changing public strategy interfaces.
- [ ] Implement DOD-focused improvements in DataProcessor order book ingestion and LiquidityTimeSeriesEngine interaction as outlined in data_pipeline_dod docs, keeping GUI-core boundaries intact.
- [ ] Establish repeatable benchmarks, guard high-risk changes with flags if needed, add debug-only assertions, and update docs to describe the final DOD-optimized data pipeline.