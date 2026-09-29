# Plan: GPU-binned heatmap in the main chart, and removal of the page/re-band path

Status 2026-09-29: S1, S2 and S4 landed; TPO profiles landed. Next: legacy phase 2 (L), then S3. See section 4.

## Owner decisions (2026-09-29, tick contract revised the same afternoon)

These decisions override every other statement in this plan and in older research notes.
Where a choice is still open, benchmark it. Do not invent a product rule.

Model: the server sends reusable, immutable chunks of real liquidity data. The client
composes time when that is cheap. The GPU aggregates price into the selected heatmap grid.
The server never builds viewport-specific pages.

**ACCEPTED**
- Reusable immutable chunks. GPU price aggregation.
- Raw pristine L2 as the durable source of truth (storage track, `2026-09-storage-pyramid.md`).
- Server serving levels 1s / 1m / 1h, derived from raw L2. The 1s level and the raw source
  depend on the pristine capture. Sub-minute history (100 ms, 1 s, 5 s, 10 s) comes later.
  The 1m floor is not permanent.
- A future exact aggregate representation (A = quantity x time integral, D = coverage
  duration, P = peak). It is the next storage generation. It does not block S3-S8.
- Active/spare source swap as a hard UX requirement: the old source keeps drawing while the
  new one builds; swap only when ready. Never blank, stretch or destroy the current view.
  Fast switching between recent timeframes is a major goal.
- About 2 ms for a full-day deep 1m re-bin. The < 1 ms target is not a product requirement.
- Multi-chart and multi-symbol: a process-wide immutable chunk cache with a global RAM budget,
  plus a per-chart controller and per-chart render/GPU state. No redundant decode per chart.
- Windows D3D11 testing by the owner (`docs/WINDOWS_GPU_TESTS.md`).

**REJECTED**
- Aggregating a block of rows by anything other than its defined statistic (for example drawing
  only the strongest constituent row): it changes what a cell means.
- Silent 1m -> 5m switches for GPU budget (and any automatic timeframe change on zoom).
- Near/deep as permanent product or renderer semantics. They may exist only during migration.
- Permanent parallel architectures (page path, re-band path, near/deep path and GPU path side
  by side). Temporary toggles only, then delete.
- 512 MiB as a fixed GPU limit. It is a safety default.

**NEEDS BENCHMARK / DESIGN**
- Whole-chunk render-ready price aggregation vs viewport clipping (slice B1).
- GPU and RAM budgets for one large chart, several charts, active+spare, and whole-chunk caches (B1).
- The per-asset default tick ladder.
- True pristine L2 rate; raw deltas + keyframes vs full snapshots every second; 100 ms
  retention; the long-term aggregate schema (storage doc).

**Heatmap product contract** (full detail: `2026-09-heatmap-interaction-spec.md`)
- Three separate concepts: source resolution (stored L2 and serving level), heatmap
  timeframe, and heatmap price tick.
- **Timeframe:** a column is exactly the selected timeframe. Zoom and pan never change the
  timeframe (no auto-timeframe). Time zoom-out clamps at one column per screen pixel.
- **Tick modes.** *Auto* (default): the tick follows zoom, the smallest preset at least
  `minRowPx` (2 px) tall, with hysteresis so thresholds do not twitch; zooming in reaches the
  finest preset the data supports (BTC $1). *Manual*: the user locks a preset; zoom only
  scales, and price zoom-out clamps at one row per pixel.
- **Presets:** `{1, 2, 2.5, 5} x 10^k` in price units, restricted to multiples of the
  `commonTick()` of the data in view (S4). BTC offers $1 upward where $1 data exists; older
  deep history recorded on a $10 grid only offers multiples of $10. Presets the visible data
  cannot build are not offered.
- **Cell meaning:** the summed time-weighted liquidity of its constituent rows (the defined
  aggregate). Coarser ticks never substitute a single row.
- **Re-bin triggers:** a tick change (Auto threshold crossing or Manual choice), a timeframe
  change, new or revised source data, new chunks, a source generation or config change, or the
  view leaving the prepared region at the same tick. Pan inside the prepared region never
  re-bins. A re-bin is a GPU pass in the same frame: no freeze.
- **Transitions:** the active source keeps drawing until its replacement is ready. Whether a
  crossfade between ticks is needed is decided in the lab (spec experiment E2), not assumed.
- Detail comes from source availability, timeframe, tick and serving level, not from a
  near/deep choice.

**Code to reshape in slice T** (no code changes in this revision):
- `libs/core/heatmap/HeatmapResolution.hpp`: keep `idealTick` as the Auto rule but add
  hysteresis and the `commonTick()` restriction; `layerFor` (near/deep) is migration-only;
  delete `autoTimeframe` / `kAutoTimeframes`.
- `libs/gui/render/RecordingBandPolicy.hpp`: the legacy page path's 2 px rule (with its
  round-trip freeze). Deleted in S8.
- `libs/gui/render/heatmap/HeatmapRenderNode` `TickPolicy`: Auto (`minRowPx` + hysteresis)
  and Manual (`manualTick`) modes.
- Lab: add the Auto/Manual toggle and hysteresis; remove auto-timeframe on zoom.

**Resolved. Do not solve these again.**
- Compute inside `QSGRenderNode::prepare()` works on Metal (S4 spike, `QsgComputeSpikeTests`).
  The `beforeRendering` fallback is not needed.
- Exact float-float codes under Metal fast math: XOR guard in the kernel, plus a runtime
  precision self-test per backend and device with a `precise` fallback (S4).
- D3D11 buffer limits: entries are paged into at most 8 buffers of at most 64 MiB, and every
  other source buffer is at most 128 MiB. HLSL 5.0 compiles. Only the D3D11 **run** is open.
- Raw-only performance: measured by the S4 bench on the compose-then-bin design.

## 0. Background facts (2026-09-28)

- **Compose time once, bin price per re-bin.** A worker composes columns at exactly the
  selected timeframe from the finest stored level that divides it. The GPU bins price only,
  one source column per output column. This is not a hidden LOD: the composed column is the
  displayed column. (The earlier lab shaders with hidden LODs and the O(tf^2) raw-only kernel
  are gone.)
- **Hour rollups exist only for deep today.** They are schema 4, with per-entry `coveredMs`
  and `CoverageRun`s (`BookRecorder` sets `hourlyRollup`). This is a fact of the HMC2 migration
  source, not a product rule.
- **Labels and walls read CPU values.** `HeatmapLabelRenderer::buildLabelGlyphs` reads
  `HeatmapStreamState::Snapshot`; walls come from `ColumnWindow::captureWalls`. The CPU
  reference `binCell`/`binColumn` stays a first-class component.
- **Machine budget.** Only one agent builds or benchmarks at a time. "Parallel" in this plan
  means different files, not simultaneous builds.

## 1. Target architecture

**Core** (`libs/core/heatmap/`, QtCore only; landed in S1):
- `SparseColumns`: per column `bucketStartMs`, `observedMs`, flags, grid id (`configHash`,
  `rowTickUnits`), `SizeScale`, coverage runs per side. Entries: packed row/side plus a 15-bit
  code; `coveredMs` for hour and composed columns.
- `TimeComposer::compose(levelColumns, tfMs)`: exact epoch-aligned buckets from minute
  records, hour records, or both. Sums covered duration and decoded mean x ms; merges coverage
  by an endpoint sweep.
- `binCell` / `binColumn` CPU reference for parity, labels and walls.
- `ChunkCodec` (binary encode/decode, zstd). `ChunkDiskCache` (LRU, S5).
- `HeatmapResolution.hpp`: **to be reshaped in T** into per-asset tick presets, the
  per-timeframe default tick, and the zoom clamp limits. No function in it may choose tick or
  timeframe from viewport geometry.

**Server** (`libs/core/servermodel/RecordingChunks.{hpp,cpp}`, landed in S2):
- `buildChunk(Hmc2Reader&, ChunkKey)` returns `SparseColumns` for one chunk span; an LRU of
  encoded sealed chunks.
- The live edge reuses `LiveCache::Snapshot` (`provisional`, `committed`,
  `committedThroughMs`, `revision`) and sends raw minute records. `LiveBuilder` and its band
  projection go away.
- Later: server serving levels 1s / 1m / 1h built from raw pristine L2 instead of HMC2.
  The chunk contract stays the same.

**GUI:**
- `render/heatmap/HeatmapGpuBinner`, `HeatmapGpuSource`, `HeatmapBinGrid`, `HeatmapRenderNode`
  (landed in S4).
- Process-wide chunk cache: disk cache and decoded LRU with one global RAM budget, shared by
  every chart and symbol.
- `HeatmapSourceController`, one per chart, a QObject on a worker QThread. It:
  - holds the user-selected timeframe and tick (no tick or timeframe from the viewport),
  - computes the needed chunks plus prefetch for the visible time range,
  - requests, decodes and composes them at the timeframe (client when cheap; otherwise it
    requests server-composed chunks, S9),
  - builds the render-ready source: whole useful price extent of each chunk, or rows clipped to
    the price window plus margin. **B1 decides which.**
  - keeps recent timeframes' sources within budget, so a switch back is fast,
  - publishes an immutable `shared_ptr<const GpuSource>` into the active/spare swap,
  - serves label cells and walls through `binCell`.

**Threads:**
- Asio client thread receives frames.
- Queued signal to the controller worker, which decodes and composes (a small pool).
- Mutex swap slot read by `UnifiedGridRenderer::updatePaintNode` on the render thread.
- The render node uploads and dispatches compute in `prepare()` and draws in `render()`.

**Data flow:** stored source (HMC2 today; raw-derived serving levels later) -> server
`buildChunk` -> binary frame -> process-wide disk cache and decoded LRU -> per-chart compose
at tf -> render-ready source -> GPU pages -> bin at the selected tick -> draw through the
camera mapping.
- Zoom and pan change only the draw mapping.
- Compute re-runs only on the re-bin triggers in the owner contract. The S4 grid is
  screen-sized with a 2-bin guard, so today leaving the guard (by pan or zoom-out) re-bins at
  the same tick. B1 decides whether that grid stays or a whole-chunk grid replaces it.

**Live edge:**
- The current hour's chunk is "open": it carries `committedThroughMs` and a revision.
- `heatmap_live_column` frames replace the provisional minute by revision and finalize it once.
- The open tf column = committed minutes of that bucket + the provisional minute, flagged provisional.
- For tf >= 1h, the open hour is composed from minutes.

**Validity and veil** (four states, S4): *data*; *veil* (scanned but unproven, or an
incompatible grid); *loading* (not scanned yet, or outside the row clip), drawn as a blue
hatch; *no data* (older than the advertised `oldestMs`), not drawn. No stale picture is ever
drawn stretched.

**Candle and overlay alignment contract:**
- `TimeAxisMapping` becomes viewport-only. `gridWidth`, `srcRect`, `timeOffset` and
  `dataStart` become heatmap-internal.
- Heatmap column k spans `[floor(t/tf)*tf, +tf)` in UTC epoch time. Candles use the same rule,
  including odd timeframes such as 16m.
- The invariant "1 slice = 1 bar" holds for tf >= 1m.

**Hosting (decided):** a `QSGRenderNode` as the first child of UGR's root; the root becomes a
plain `QSGNode`, replacing `ensureHeatmapRootNode`'s `HeatmapIntensityNode`. It draws below
the footprint/TPO/label/text children, uses the same frame's mapping, and needs no offscreen
texture. `QQuickRhiItem` is rejected for the main chart (offscreen target, composite pass,
possible one-frame misalignment).

## 2. Wire protocol

- **Text JSON, requests only:**
  - `heatmap_chunk_request {req, symbol, layer, level_ms, starts[], have_hash[]}`
  - `heatmap_live_subscribe {symbol, layers[]}`
  - Server response `heatmap_availability {layers: {oldest, latest, native grids/generations, levels}}`,
    sent on subscribe and whenever it changes.
  - `layer` is migration-only. It names an HMC2 layer while HMC2 is the source. With
    raw-derived levels the key is source generation/grid plus level.
- **Binary frames:**
  - Cached chunk body: magic `SHC1`, wire version, kind (`chunk | live_column | not_modified | error`),
    chunk key, grid id, `SizeScale`, state (`sealed`, or `open{committedThroughMs, revision}`),
    content hash, columns, entries, payload length. Request id belongs to a separate `SHE1`
    envelope so cached bytes can serve any request.
  - Payload: zstd over column records `{bucket, observedMs, flags, coverage runs, n}`, then
    entries column by column (row-delta varints, side bits, u16 codes, `coveredMs` for hours).
  - Server: add `bool binary` to `Session::PendingWrite` and call `ws_.binary(...)` in
    `internal_async_write`. Client: branch on `m_ws.got_binary()` in `onRead`.
- **Identity:**
  - Key: `(symbol, layer, levelMs, startMs)` with a fixed span per level: 1m level -> 1 UTC
    hour; 1h level -> 1 UTC day. A future 1s level needs its own span (to be measured).
  - Chunk span is open to B1: MarketLens uses 64-column, epoch-aligned, power-of-two chunks
    (64 min at 1m; 16-237 KB per market-chunk after pcodec). Sentinel's 1m hour chunk is 60
    columns (S2: 82 KB deep). A 64-column span makes chunk ids trivial at every timeframe.
  - A chunk is sealed only once its native-level recorder watermark reaches end. A sealed
    chunk never changes. A config change produces new `configHash` values, never an edited chunk.
- **Versioning:** `recording.chunk_wire_version` in `server_config`. On a mismatch the client
  refuses and wipes its cache. No compatibility shims.
- **Disk cache:** `<CacheLocation>/Sentinel/heatmap-chunks/<server-id>/...`, the exact wire
  payload, sealed chunks only, shared by all charts.
- **Budgets:** starting values only. The final policy is measured (B1): configurable,
  dynamic, process-global or per-chart. A process-global budget with per-chart working sets is
  a candidate.

| What | Starting value |
|---|---|
| Disk cache (process-wide) | 2 GiB LRU |
| Decoded RAM LRU (process-wide) | 512 MiB |
| GPU source buffers | 512 MiB cap on active + spare per binner (S4 safety default) |
| Output grid | <= 66 MiB |
| Client in flight | 4 chunks / 16 MiB |
| Server | existing two-worker, eight-job pool |

- **Prefetch:** visible range +/-1 viewport width at the current level, plus hour-level chunks
  for 3x the visible span in the background, so tf switches stay local. (MarketLens prefetches
  about one chunk beyond the visible range; B1 and S5 measure which margin is enough.)
- **Composition split:** the client composes a timeframe when the fine data it needs is cheap
  to hold; otherwise the server composes and caches tf chunks (S9). Persistent serving levels
  are server-side only.

## 3. Delete and keep

**Delete:**

*Server and core*
- `RecordingPage.{hpp,cpp}` (`buildPage`, `BuildRequest`, `BuildResult`, `PriceBand`). Freeze goldens first.
- `RecordingLive` `LiveBuilder`/`LiveView` band projection.
- In `RecordingHistoryWire.hpp`: `parseRequest`, `buildRequest`, `emptyPaddingValues`,
  `encodeU16`, `padValidity`, `buildChunk`, `parseView`, `buildLive`, `viewError`, `error`.
- `SentinelStreamServer` handlers for `heatmap_recording_view` and `heatmap_history_request`
  with `source=recording`.
- `MessageType::HeatmapRecordingLive`.
- `HeatmapResolution.hpp` `idealTick`, `layerFor` and `autoTimeframe` (replaced in T).
- Near/deep as a product concept, once the source is raw-derived levels.

*Client*
- `SentinelStreamClient::registerRecordingView`, `requestRecordingHeatmapHistory`,
  `RecordingHistoryPage`, the `recording*` signals, and their `IGridDataSource`/`RemoteGridDataSource` counterparts.
- `DataProcessor`:
  - Recording: `scheduleRecordingBand`, `applyRecordingBand`, `sendRecordingRequest`, `resetRecordingRequest`, `onRecording*`.
  - Heatmap window: `setHeatmapViewport`, `ensure/reset/publishHeatmapWindow`,
    `requestHeatmapFetch`, `captureHeatmapWalls`, `heatmapWindowUpdated`, and all `m_recording*` state.
- Whole files: `HeatmapColumnWindow.{hpp,cpp}` and `RecordingBandPolicy.hpp` (its tick rule is
  not carried over).
- `HeatmapRenderNode::TickPolicy::minRowPx` fallback (T).
- `HeatmapStreamState` ring, and from `HeatmapStreamService` the texture parts
  (`rebuildTextureFromRing`, ingest, the texture half of `handleTimeframeChange`, `setGridDimensions`).
- `HeatmapOverlayRenderer`, including the 8192x2048 band (`server_config` `grid_width: 8192`,
  `grid_height: 2048`) and `updateHistoryGapNode`.

*Tests*
- `tests/render/test_HeatmapColumnWindow.cpp`, `test_RecordingDataProcessor.cpp`, `test_RecordingBandPolicy.cpp`
- `tests/marketdata/test_recording_history_wire.cpp`
- `tests/servermodel/test_recording_page.cpp` -> becomes a golden test
- The `LiveBuilder` parts of `test_recording_live.cpp`, and `recording_live_bench.cpp`

*Docs*
- The "Recording heatmap re-band publication" section in ARCHITECTURE.md.

**Keep:**
- `Hmc2Store`/`Hmc2Reader`, `BookRecorder`, `LiveCache`/`LiveService` (with raw delivery),
  `PriceLadder`, `RecordingCodec`, `threadReader`, `capability` (until raw-derived levels replace HMC2).
- `HeatmapIntensityNode`/`HeatmapColumnTexture`: footprint and TPO still use them.
- The auto-scroll and price-centering parts of `HeatmapStreamService`, moved into the viewport controller.

**Rollout toggle:** client `heatmap.renderer: gpu|page`, only during S6-S7. S8 removes it.

## 4. Slices

| # | Slice | Files | Acceptance | Status |
|---|---|---|---|---|
| L | Legacy phase 2 (inventory doc) | DataProcessor.cpp, MainWindowGpu.cpp, SentinelStreamServer.cpp | Inventory checklist. Land before S3. | **Next** |
| S1 | Model: `SparseColumns`, `TimeComposer`, `binCell` | new core files + tests | vs `buildPage`: 0 log-code-step deviation, equal validity over 1m-1D, gaps, grid changes, partial coverage. | Landed (`d382215`) |
| S2 | Server chunks: `RecordingChunks` + `ChunkCodec` | new core files | Round-trip exact; real-root bench (below). | Landed (`07b61b1`) |
| S3 | Wire: binary framing and new messages | SentinelStreamServer/Client, SentinelStreamProtocol.hpp | Loopback TLS test: chunk, not_modified, live revision ordering, backlog bounds, stop/teardown. | **Next**, after L |
| S4 | GPU: binner, render node, shaders; lab repointed | libs/gui/render/heatmap, sentinel-lab | Readback vs `binCell` exact. About 2 ms full-day deep 1m re-bin accepted by the owner. D3D11 run by the owner. | Landed (`4c5a991`); D3D11 run open |
| B1 | **Benchmark: whole-chunk render-ready price aggregation vs viewport clipping** | libs/gui/lab (Bench, LabSources) | See below. Decides the render-ready source shape and the budget policy. | Before S5 finalizes |
| T | Tick and zoom contract | HeatmapResolution.hpp, HeatmapRenderNode, lab | Auto (default, 2 px + hysteresis) and Manual tick modes; presets multiples of `commonTick()` (BTC from $1); column = timeframe, time clamp 1 column/px; Manual price clamp 1 row/px; no auto-timeframe. Lab experiments E1-E3 of the interaction spec. Unit tests. | Before S5 |
| S5 | Controller: `HeatmapSourceController` + process-wide `ChunkDiskCache` | new files, fake-transport tests | Chunk set and prefetch per viewport; global RAM budget shared by two charts with no duplicate decode; active/spare swap never blanks; switch to a recently used tf < 50 ms from cache; reconnect resume. | After B1, T |
| S6 | Integration behind toggle | UGR*, MainWindowGpu.cpp, FrameContextBuilder, UgrFrameMath, TimeAxisMapping | Cold start <= 1 s to first data frame. Zero full-texture rebuilds. Zoom repaints in the next frame with no tick or tf change. No stretched columns on tf switch. A/B screenshots via Agent API. | |
| S7 | Labels/walls from `binCell`; tick selector UI | UGR*, GuiApiServer, TopToolbar | Walls API parity with the old path; label glyph tests. | |
| S8 | Deletion (section 3), including near/deep product paths | DataProcessor.cpp and the rest | Parity tests run against goldens; full ctest; docs updated. | |
| S9 | Server-composed tf chunks | RecordingChunks, SentinelStreamServer | Used when client composition is too expensive; parity with client compose. | |

**B1 (owner item 8).** Pipeline under test: fetch chunk -> compose timeframe -> aggregate the
**entire useful price extent** of the chunk at the selected tick -> cache the render-ready binned
chunk -> the viewport only selects the visible part. Compare against the current viewport-clipped
source with the guard grid, at fixed preset ticks (not the per-zoom `idealTick` of the S4 bench).
Measure: GPU memory, CPU memory, upload time, bin time, vertical pan latency, horizontal pan
latency, chunk reuse, multi-chart memory, and the cost when a source chunk arrives or updates.
In the same harness, measure the budget inputs (owner item 9): one large chart, several charts,
active+spare, whole-chunk caches. Prefer whole-chunk if it is affordable. Do not assume it wins.

Prior art for B1 (`2026-09-marketlens-har.md`; behaviour only, not a claim about unobservable internals):
- *Observed:* every heatmap request carries a fixed timeframe and price step (`dt=60000`,
  `dp=10.0` for BTC at 1m). Zoom never changed them. History arrives as epoch-aligned chunks
  (order books 64 min, trades and footprints 128 min), newest first, with about one chunk of
  prefetch. The GPU holds 256x256 `R32F` tiles, uploads only modified tiles, and only
  colour-maps; pan and zoom are uniforms. Live updates arrive by WebSocket every 100 ms into
  the current column.
- *Inferred:* the client (WASM, CPU) aggregates whole chunks into those cached render-ready
  tiles. That is the whole-chunk model B1 tests, but with CPU aggregation instead of GPU.
- *Observed failure:* the browser HTTP cache served stale partial open chunks and empty
  "future" chunks. This supports Sentinel's rule to cache sealed chunks only.

**Storage track (not on this critical path):** the pristine ingest capture and the raw-vs-snapshot
benchmark (`2026-09-storage-pyramid.md`). The server 1s level and the raw source follow from it.

- **Order:** L -> S3; B1 and T (lab and core files) -> S5 -> S6 -> S7 -> S8, then S9.
- **Parallel by files:** B1 and T with L and S3. Only one of them builds at a time.
- **Serial:** S3/L (SentinelStreamServer.cpp) and S6/S7/S8 (hot GUI files).

S2 opt-in real-recording benchmark (`SENTINEL_CHUNK_BENCH=1`, latest complete 24 hours,
2026-09-29, arm64 Mac, revised v1): deep 82,294 encoded bytes/hour, 7.40 ms encode, 5.59 ms
decode, 24.19 ms cold reader; near 177,320 bytes/hour, 3.12 ms encode, 1.97 ms decode, 26.74 ms
cold reader. Bytes are the cacheable SHC1 body; a request envelope adds 14 bytes per response.

**S4 spike (2026-09-29, Qt 6.11.2, Metal, arm64 Mac): a compute pass can be recorded in `QSGRenderNode::prepare()`.**
- Evidence: `tests/render/test_qsg_compute_spike.cpp` (ctest `QsgComputeSpikeTests`). A
  `QSGRenderNode` in an ordinary `QQuickItem` tree calls `beginComputePass()`/`dispatch()`/`endComputePass()`
  in `prepare()`, writing a storage buffer, and reads it in its fragment shader in `render()` in
  the same frame. Pixels equal the compute output; the rest of the scene is intact; 20/20 repeats
  pass with the Metal API validation layer on.
- Why: the batch renderer calls every render node's `prepare()` before the main render pass
  begins, so the command buffer is outside a pass.
- The test renders through `QQuickRenderControl` into a Metal texture (`libs/gui/lab/OffscreenQuick`),
  so it runs headless. Under the offscreen QPA the harness calls `QQuickWindow::setSceneGraphBackend("rhi")`.

**S4 result (`lt-claude/s4-gpu-binner`).** Code in `libs/gui/render/heatmap/` (target `sentinel_heatmap_gpu`):
- `HeatmapGpuSource`: a worker-built immutable upload image of one composed `SparseColumns`.
  Time slots mark each bucket as column, `NotLoaded` or `Gap`. Same-tick constituents pool as in
  `binColumn`. Each entry is `numerator / pooled coverage * group weight` in double, shipped as
  float-float (about 2^-38 relative error), 8 bytes (12 if a group's row span exceeds 16 bits).
  Coverage is merged full-coverage runs; a row index (one per 16 rows) finds a bin's first entry.
- `HeatmapGpuBinner`: pages sources in within a per-frame byte budget; the active source keeps
  drawing until the new one is complete. Output is one `uint` per cell (code, side, state).
- `HeatmapBinGrid`: absolute bucket and price-bin anchoring with a 2-bin guard.
- `HeatmapRenderNode`: compute in `prepare()`, draw in `render()`. While a new timeframe uploads,
  the grid uses the active source's timeframe, so old columns keep their true time extent.

Findings the later slices need:
- **Exact codes need float-float, and Metal fast math breaks naive float-float.** GLSL `precise`
  survives but doubles kernel time. The kernel passes every intermediate through an XOR with a
  runtime zero:

  | Kernel | Code mismatches, 10.2M real cells | Code mismatches, stress test |
  |---|---|---|
  | float-only sums | 381 | not run |
  | float-float without the XOR | 381 | 9 |
  | shipped (XOR laundering) | 0 | 0 |

- **Display ticks must be multiples of `commonTick()`**, the LCM of a source's native ticks. The
  last deep day mixes $10 and $5 grids. T's preset ladder must respect this.
- **Parity:** synthetic 529k cells (gaps, grid and size-scale changes, NotLoaded, hour+minute,
  row clip, incompatible tick); stress 728k cells; real data opt-in (`SENTINEL_HEATMAP_REAL_PARITY=1`,
  previous closed UTC day, deep 1m/5m/1h and near 1m) 10.2M cells. Zero state, code, side or
  validity mismatches against `bucketState` + `binColumn`.
- **Bench** (`sentinel-lab --bench`, base Apple M4, last 24 h, 25 zoom levels x 200 passes, GPU
  timestamps). The tick at each zoom level came from the 2 px `idealTick` rule, now rejected; B1
  re-measures at fixed preset ticks.

  | Case | Entries | GPU source | 1x p95 / max (ms) | 2x p95 / max (ms) |
  |---|---|---|---|---|
  | deep 1m | 20.4M | 178 MB | 1.96 / 2.27 | 1.93 / 2.45 |
  | deep 5m | 4.1M | 36 MB | 0.56 / 0.67 | 0.52 / 0.59 |
  | deep 1h | 0.35M | 3 MB | 0.06 / 0.10 | 0.05 / 0.07 |
  | near 1m | 6.5M | 55 MB | 0.93 / 1.13 | 1.09 / 1.38 |

  - Cost is linear in visible entries. The full-day level reads 17.9M entries (143 MB) in
    2.27 ms; the bandwidth floor is about 1.2 ms. The owner accepts about 2 ms per re-bin; there
    is no automatic 1m -> 5m step-up.
  - Kept kernel changes: compensated accumulation with one renormalization, the row index, a
    log-estimated code corrected against the threshold table. Rejected (slower): strip-walking
    cursors (3.7 ms), interpolation or galloping search (4.7 ms).
- **Review fixes:**
  - *D3D11 buffer size:* entries in up to 8 pages of <= 64 MiB; a native-tick group never
    straddles a page, so the kernel picks the page once per group. Other source buffers <= 128 MiB.
    The deep day uses 3 pages (deep 1m 1x p95 2.02 ms, near 1m 0.88 ms).
  - *Allocation:* two grow-only buffer sets (active and spare), at most one pending source. A
    failed allocation or refused source drops only the pending source. A -> B -> A cancels the
    pending upload. Refused sources are reported once and not retried every frame (allocation
    failures back off 2 s, doubling to 60 s).
  - *GPU memory cap:* active + spare capped at 512 MiB by default (`Frame::gpuMemoryCapBytes`).
    This is a safety default, not policy (owner decision); B1 measures the real need.
  - *Re-bin key:* source id, grid coverage, output size scale, kernel variant.
  - *Display tick:* chosen for the source actually drawn (`TickPolicy`), never for a pending one.
    Its `minRowPx` fallback is the rejected viewport rule (T).
  - *Runtime precision self-test* (`HeatmapGpuSelfTest`), once per QRhi backend and device: 384
    cells (128 adversarial) against `binColumn`. The binner uses the `precise` kernel until the test
    passes. The M4 passes; a folding kernel fails 128/384 and is rejected. Precise fallback: p95
    3.22 ms vs 2.02 ms fast (deep 1m, 1x). The fixture is built once per process on a worker.
- **Unverified:**
  - The on-screen interactive lab window (the screen was locked).
  - D3D11 at run time. The GPU tests and the lab create a Metal QRhi only, so on Windows they
    skip. See `docs/WINDOWS_GPU_TESTS.md`.
  - Memory at 2x with several charts. The spare set keeps the capacity of the largest earlier
    source (up to about 2x the source bytes). B1 measures it.

## 5. Risks and open questions

Resolved items are listed in the owner-decisions section above. Open:

1. **D3D11 run.** Paging and HLSL compilation are done. No D3D11 run exists: the test harness
   is Metal-only. A small harness change must come first (`docs/WINDOWS_GPU_TESTS.md`).
2. **Memory on a 16 GB Mac.** At 1 px/column, 1m tf and 3840 columns, deep is about 44M raw
   entries (about 265 MB). The zoom-out clamp bounds the visible grid; B1 measures whole-chunk,
   active+spare and multi-chart memory. Beyond budget, the user picks a coarser tf or tick, or
   S9 composes on the server.
3. **HMC2 stores a quantized mean, not the time integral.** The half-code relative quantization
   error of the log encoding is about 0.0423% (one code step is about 0.085%); it is not a
   cumulative drift. Decode -> aggregate -> re-encode cannot be exact. S1 keeps composed
   numerators in double, so client composition does not re-quantize. The exact A/D/P
   representation is the next storage generation and does not block S3-S8.
4. **Odd timeframes.** 90m is not an hour multiple, so it composes from 90 minutes per column.
5. **Sub-minute timeframes.** The heatmap must not disappear when 1s candles have valid L2
   underneath. Open: what to draw at a sub-minute tf for history before a sub-minute level exists.
6. **Per-asset default tick ladder.** Open (owner). Constraint: multiples of `commonTick()`.
7. **WAN latency for remote clients.** Chunk bytes are known (S2); latency is not measured.
8. **Cold-start critical path.** TLS handshake + `server_config` + cold `Hmc2Reader` discovery +
   1-2 deep hour chunks. The newest chunk is requested first and painted on arrival.

### Critical files for implementation
- libs/core/heatmap/HeatmapResolution.hpp, libs/gui/render/RecordingBandPolicy.hpp (tick policy, T and S8)
- libs/gui/render/heatmap/HeatmapRenderNode.cpp, HeatmapGpuBinner.cpp, HeatmapBinGrid.hpp (B1, S6)
- libs/gui/lab/Bench.cpp, LabSources.cpp (B1)
- libs/core/servermodel/RecordingChunks.cpp, libs/core/protocol/SentinelStreamServer.cpp (S3, S9)
- libs/gui/UnifiedGridRenderer.Render.cpp, libs/gui/render/DataProcessor.cpp (S6-S8)
