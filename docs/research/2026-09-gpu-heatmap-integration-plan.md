# Plan: GPU-binned heatmap in the main chart, and removal of the page/re-band path

## 0. Facts that change the design

- **The lab numbers came from shaders that use hidden LODs.** Commit `7ac2190` bins with an 8-minute time LOD and 10/40/100-row dense price LODs. That conflicts with the owner's rules. The uncommitted working tree in `lt-sol-gpu-bin-lab` switches `bin.comp` to raw-only, but nobody has measured it.
- **The raw-only shader has a cost problem.** Its `partialCoverageSums` is O(tf²) per cell, so it cannot serve 1h or 1D straight from minute records.
- **Fix: compose time once, bin price every frame.** A worker composes columns at exactly the selected timeframe, summing duration-weighted values from the finest stored level that divides it. The GPU then bins price only, one source column per output column. This is not a hidden LOD, because the composed column is the displayed column.
- **Hour rollups exist only for deep.** They are schema 4, with per-entry `coveredMs` and `CoverageRun`s (`BookRecorder` sets `hourlyRollup`). `buildPage` forces deep for tf ≥ 1h and reads minute records for the current hour. Keep both rules.
- **Labels and walls read CPU values.** `HeatmapLabelRenderer::buildLabelGlyphs` reads `HeatmapStreamState::Snapshot`, and walls come from `ColumnWindow::captureWalls`. A CPU reference binner stays a first-class component.
- **Machine budget.** AGENTS.md (`deea17a`) allows only one agent that builds or benchmarks at a time. In this plan, "parallel" means the slices touch different files, not that they build at the same time.

## 1. Target architecture

**Core** (`libs/core/heatmap/`, new; QtCore only):
- `SparseColumns`: promoted from the lab's `recording::RecordingEntries`.
  - Per column: `bucketStartMs`, `observedMs`, flags, grid id (`configHash`, `rowTickUnits`), `SizeScale`, and coverage runs per side.
  - Entries: packed row/side plus a 15-bit code; `coveredMs` for hour and composed columns.
  - The disk loader moves to test helpers.
- `TimeComposer`:
  - `compose(levelColumns, tfMs)` makes exact epoch-aligned buckets from minute records, hour records, or both (the open hour uses the minute tail).
  - Each entry sums covered duration and decoded mean × ms. Coverage runs are merged by an endpoint sweep, following the rules in RecordingPage.cpp `add`/`finish`.
- `binCell` CPU reference, used for parity, labels and walls.
- `ChunkCodec` (binary encode/decode, zstd).
- `ChunkDiskCache` (LRU).
- `HeatmapResolution.hpp`:
  - `idealTick`, moved out of `RecordingBandPolicy.hpp` and built on `PriceLadder.hpp` `ladderTick`.
  - The layer rule: `tick < deepTick ? near : deep`, and deep when tf ≥ 1h.
  - Auto-timeframe selection.

**Server** (`libs/core/servermodel/RecordingChunks.{hpp,cpp}`, new):
- `buildChunk(Hmc2Reader&, ChunkKey)` runs `Hmc2Reader::visit` over one chunk span and returns `SparseColumns`.
- An LRU of encoded sealed chunks.
- The live edge reuses `LiveCache::Snapshot` (`provisional`, `committed`, `committedThroughMs`, `revision`) and sends raw minute records. `LiveBuilder` and its band projection go away.

**GUI:**
- `render/heatmap/HeatmapGpuBinner`: productionized from lab `GpuBinner`, with a simplified shader and paged uploads (lab style, 2 MiB per frame; the old source keeps drawing until the new one is complete).
- `HeatmapRenderNode`.
- `HeatmapSourceController`, a QObject on its own worker QThread. For each viewport (time and price range, px size, tf) it:
  - picks tick and layer,
  - computes the needed chunks plus prefetch,
  - requests, decodes and composes them,
  - clips rows to the price window plus margin,
  - publishes an immutable `shared_ptr<const GpuSourceSnapshot>`.
  - It also serves label cells and walls through `binCell`.

**Threads:**
- Asio client thread receives frames.
- Queued signal to the controller worker, which decodes and composes (a small pool).
- Mutex swap slot read by `UnifiedGridRenderer::updatePaintNode` on the render thread.
- The render node uploads and dispatches compute in `prepare()` and draws in `render()`.

**Data flow:** HMC2 files → server `buildChunk` → binary frame → disk cache and decoded LRU → compose at tf → row clip → GPU pages → compute bin into a screen-sized grid with an absolute bin origin → draw with sub-bin offset.
- Panning inside the current bins changes only the draw mapping.
- Compute re-runs only when the source version or the grid key changes (first bucket, tf, row origin, tick group, columns, rows).

**Live edge:**
- The current hour's chunk is "open": it carries `committedThroughMs` and a revision.
- `heatmap_live_column` frames replace the provisional minute by revision and finalize it once.
- The open tf column = committed minutes of that bucket + the provisional minute, flagged provisional.
- For tf ≥ 1h, the open hour is composed from deep minutes.

**Validity and veil** (three states, each drawn differently):
- A cell is valid only if coverage proves both sides over its whole row range.
- An invalid cell inside a sealed, scanned chunk draws the veil (lab `display.frag`, `value.z < 0.5`).
- A chunk not yet loaded draws as "loading", which is visibly different from the veil.
- Anything older than the advertised `oldestMs` draws as "no data".
- No stale picture is ever drawn stretched.

**Candle and overlay alignment contract:**
- `TimeAxisMapping` becomes viewport-only. Today `computeAndApplyFrameMapping` makes `valid` depend on the ring's `timeOriginMs` and `gridWidth`. `gridWidth`, `srcRect`, `timeOffset` and `dataStart` become heatmap-internal.
- Heatmap column k spans `[floor(t/tf)*tf, +tf)` in UTC epoch time. Candles must use the same rule, including odd timeframes such as 16m (the candle-backfill workstream composes these from 1m).
- The invariant "1 slice = 1 bar" holds for tf ≥ 1m.

**Hosting options:**
- **Recommended: a `QSGRenderNode` as the first child of UGR's root.** The root becomes a plain `QSGNode`, replacing `ensureHeatmapRootNode`'s `HeatmapIntensityNode`.
  - It draws in line, below the footprint/TPO/label/text children that UGR already parents.
  - It uses the same frame's mapping, so there is no lag against candles.
  - There is no offscreen texture.
- **Fallback: compute in `QQuickWindow::beforeRendering`,** writing an RGBA32F storage texture that a `QSGGeometryNode` material samples. This keeps the plain QSG node model.
- **Rejected for the main chart: `QQuickRhiItem`.** It needs an offscreen colour target (about 33 MB at 4K) plus a composite pass. It would have to be a separate item below UGR, and its sync order against UGR's mapping is unclear, which risks a one-frame misalignment. The lab's LabItem keeps using it as the benchmark harness, around the same binner class.

## 2. Wire protocol

- **Text JSON, requests only:**
  - `heatmap_chunk_request {req, symbol, layer, level_ms, starts[], have_hash[]}`
  - `heatmap_live_subscribe {symbol, layers[]}`
  - Server response `heatmap_availability {layers: {oldest, latest, native grids/generations, levels}}`, sent on subscribe and whenever it changes.
- **Binary frames (new):**
  - Cached chunk body: magic `SHC1`, wire version, kind (`chunk | live_column | not_modified | error`), chunk key, grid id, `SizeScale`, state (`sealed`, or `open{committedThroughMs, revision}`), content hash, columns, entries, payload length. Request id belongs to a separate `SHE1` envelope so cached bytes can serve any request.
  - Payload: zstd over column records `{bucket, observedMs, flags, coverage runs, n}` followed by entries stored column by column (row-delta varints, side bits, u16 codes, and `coveredMs` for hours).
  - Server changes: add `bool binary` to `Session::PendingWrite` and call `ws_.binary(...)` in `internal_async_write`. Client change: branch on `m_ws.got_binary()` in `onRead`.
- **Identity:**
  - Key: `(symbol, layer, levelMs, startMs)` with a fixed span per level: 1m level → 1 UTC hour; 1h level → 1 UTC day.
  - A chunk is sealed only once its native-level recorder watermark reaches end. The minute watermark already includes lateness; the hour watermark advances after hour-rollup persistence. A sealed chunk never changes.
  - A config change produces new `configHash` values per column, never an edited chunk.
- **Versioning:** `recording.chunk_wire_version` in `server_config`. On a mismatch the client refuses and wipes its cache. No compatibility shims.
- **Disk cache:** `<CacheLocation>/Sentinel/heatmap-chunks/<server-id>/…`, storing the exact wire payload, sealed chunks only. LRU default 2 GiB.
- **Budgets:**

| What | Default |
|---|---|
| Decoded RAM LRU | 512 MiB |
| GPU source | 256 MiB |
| Output grid | ≤ 66 MiB |
| Client in flight | 4 chunks / 16 MiB |
| Server | existing two-worker, eight-job pool |

- **Prefetch:** visible range ±1 viewport width at the current level, plus hour-level chunks for 3× the visible span in the background, so tf switches stay local.
- **Extreme zoom (later):** the server composes tf chunks (for example 1D) when the client's entry budget would be exceeded.

## 3. Delete and keep

**Delete:**

*Server and core*
- `RecordingPage.{hpp,cpp}` (`buildPage`, `BuildRequest`, `BuildResult`, `PriceBand`). Freeze goldens first.
- `RecordingLive` `LiveBuilder`/`LiveView` band projection.
- In `RecordingHistoryWire.hpp`: `parseRequest`, `buildRequest`, `emptyPaddingValues`, `encodeU16`, `padValidity`, `buildChunk`, `parseView`, `buildLive`, `viewError`, `error`.
- `SentinelStreamServer` handlers for `heatmap_recording_view` and `heatmap_history_request` with `source=recording`.
- `MessageType::HeatmapRecordingLive`.

*Client*
- `SentinelStreamClient::registerRecordingView`, `requestRecordingHeatmapHistory`, `RecordingHistoryPage`, and the `recording*` signals, plus their `IGridDataSource`/`RemoteGridDataSource` counterparts.
- `DataProcessor`:
  - Recording: `scheduleRecordingBand`, `applyRecordingBand`, `sendRecordingRequest`, `resetRecordingRequest`, `onRecording*`.
  - Heatmap window: `setHeatmapViewport`, `ensure/reset/publishHeatmapWindow`, `requestHeatmapFetch`, `captureHeatmapWalls`, `heatmapWindowUpdated`, and all `m_recording*` state.
- Whole files: `HeatmapColumnWindow.{hpp,cpp}` and `RecordingBandPolicy.hpp` (after its logic moves to `HeatmapResolution`).
- `HeatmapStreamState` ring, and from `HeatmapStreamService` the texture parts (`rebuildTextureFromRing`, ingest, the texture half of `handleTimeframeChange`, `setGridDimensions`).
- `HeatmapOverlayRenderer`, including the 8192×2048 band (`server_config` `grid_width: 8192`, `grid_height: 2048`) and `updateHistoryGapNode`.

*Tests*
- `tests/render/test_HeatmapColumnWindow.cpp`, `test_RecordingDataProcessor.cpp`, `test_RecordingBandPolicy.cpp`
- `tests/marketdata/test_recording_history_wire.cpp`
- `tests/servermodel/test_recording_page.cpp` → becomes a golden test
- The `LiveBuilder` parts of `test_recording_live.cpp`, and `recording_live_bench.cpp`

*Docs*
- The "Recording heatmap re-band publication" section in ARCHITECTURE.md.

**Keep:**
- `Hmc2Store`/`Hmc2Reader`, `BookRecorder`, `LiveCache`/`LiveService` (with raw delivery), `PriceLadder`, `RecordingCodec`, `threadReader`, `capability`.
- `HeatmapIntensityNode`/`HeatmapColumnTexture`: footprint and TPO still use them.
- The auto-scroll and price-centering parts of `HeatmapStreamService`, moved into the viewport controller.

**Rollout toggle:** client `heatmap.renderer: gpu|page`, only during slices S6–S7. It is removed in S8.

## 4. Slices

Each slice gets a short tag: S1 (model), S2 (server chunks), S3 (wire), S4 (GPU), S5 (controller), S6 (integration), S7 (labels/walls/zoom), S8 (deletion), S9 (server compose).

| # | Slice | Files | Acceptance |
|---|---|---|---|
| L | Legacy phase 2 (inventory doc) | DataProcessor.cpp, MainWindowGpu.cpp, SentinelStreamServer.cpp | Inventory checklist. Dispatch now; land before S3. |
| S1 | Model: `SparseColumns`, `TimeComposer`, `binCell` | new core files + tests | vs `buildPage`, 0 log-code-step deviation, validity equal: 1m, 5m, 16m, 1h, 4h, 1D, near/deep, synthetic store with gaps, grid changes, partial coverage (lab fixture). |
| S2 | Server chunks: `RecordingChunks` + `ChunkCodec` | new core files | Round-trip exact. Bench on the real root: bytes per deep hour, encode ms, cold-reader ms. Open chunk vs `LiveCache`. |
| S3 | Wire: binary framing and new messages | SentinelStreamServer/Client, SentinelStreamProtocol.hpp | Loopback TLS test: chunk, not_modified, live revision ordering, backlog bounds, stop/teardown (extend `test_recording_server_stop`). |
| S4 | GPU: `HeatmapGpuBinner`, `HeatmapRenderNode`, shaders; lab repointed | new gui files, sentinel-lab | Readback vs `binCell` exact. Compute p95 < 1 ms at 1x and < 2 ms at 2x on M4 (full deep day, 1m/5m/1h). **First task: spike compute inside `QSGRenderNode::prepare()`.** D3D11 readback run by the owner. |
| S5 | Controller: `HeatmapSourceController` + `ChunkDiskCache` | new files, fake-transport tests | Chunk set and prefetch per viewport, LRU eviction under budget, reconnect resume, tf switch with cached source < 50 ms compose. |
| S6 | Integration behind toggle | UGR*, MainWindowGpu.cpp, FrameContextBuilder, UgrFrameMath, TimeAxisMapping | Cold start: fresh server + empty cache → first data frame ≤ 1 s (log probe). Zero full-texture rebuilds. Zoom with cached source repaints in the next frame. No stretched columns on tf switch. A/B screenshots via Agent API. |
| S7 | Labels/walls from `binCell`; 1 px/column zoom clamp; auto-timeframe | UGR*, GuiApiServer, TopToolbar | Walls API parity with the old path; label glyph tests. |
| S8 | Deletion (section 3) | DataProcessor.cpp and the rest | Parity tests now run against goldens; full ctest; docs updated. |
| S9 | Server-composed tf chunks for extreme zoom | RecordingChunks, SentinelStreamServer | Budget-triggered; parity with client compose. |

S2 opt-in real-recording benchmark (`SENTINEL_CHUNK_BENCH=1`, latest complete 24 hours, 2026-09-29, arm64 Mac, revised v1): deep 82,294 encoded bytes/hour, 7.40 ms encode, 5.59 ms decode, 24.19 ms cold reader; near 177,320 bytes/hour, 3.12 ms encode, 1.97 ms decode, 26.74 ms cold reader. Bytes are the cacheable SHC1 body; a request envelope adds 14 bytes per response.

**S4 spike answer (2026-09-29, Qt 6.11.2, Metal, arm64 Mac): yes, a compute pass can be recorded in `QSGRenderNode::prepare()`.**
- Evidence: `tests/render/test_qsg_compute_spike.cpp` (ctest `QsgComputeSpikeTests`). A `QSGRenderNode` inside an ordinary `QQuickItem` tree calls `commandBuffer()->beginComputePass()`/`dispatch()`/`endComputePass()` in `prepare()`, writing a storage buffer, then reads that buffer in its fragment shader in `render()`, in the same frame. The rendered pixels equal the compute output, the rest of the scene is intact, and a second frame dispatches again. 20/20 repeats pass with the Metal API validation layer on (`METAL_DEVICE_WRAPPER_TYPE=1 MTL_DEBUG_LAYER=1`) with no validation messages.
- Why it works: the batch renderer calls every render node's `prepare()` before it begins the main render pass, so the command buffer is outside a pass, which is what `beginComputePass()` requires. The compute encoder ends before the render encoder starts, so Metal orders the buffer write before the fragment read.
- The test renders through `QQuickRenderControl` into a Metal texture (`libs/gui/lab/OffscreenQuick`), so it runs headless and with the screen locked. The same batch renderer drives an on-screen `QQuickWindow`. Under the offscreen QPA, the harness must call `QQuickWindow::setSceneGraphBackend("rhi")`, or Qt Quick selects the software adaptation.
- Result: `HeatmapRenderNode` uses this mechanism. The `beforeRendering` fallback is not needed. D3D11 is unverified: the owner runs the same test on Windows.

**S4 result (2026-09-29, `lt-claude/s4-gpu-binner`).** Code is in `libs/gui/render/heatmap/` (target `sentinel_heatmap_gpu`):
- `HeatmapGpuSource`: a worker-built, immutable upload image of one composed `SparseColumns`.
  - Time slots mark each bucket as column, `NotLoaded` or `Gap`.
  - Same-tick constituents are pooled exactly as `binColumn` pools them.
  - Each entry is a per-row value `numerator / pooled coverage * group weight`, computed in double and shipped as float-float: a float hi part plus a 15-bit low part quantized to ulp(hi)·2⁻¹⁴, which gives about 2⁻³⁸ relative error. An entry takes 8 bytes (12 bytes if a group's row span exceeds 16 bits).
  - Coverage is stored as merged full-coverage runs. A row index (one entry per 16 rows) finds the first entry of a bin.
  - The source records its advertised availability and an optional row clip.
- `HeatmapGpuBinner`:
  - Pages sources in within a per-frame byte budget, into fresh buffers. The active source keeps drawing until the new one is complete.
  - The output buffer only grows. A resize does not touch the source buffers.
  - Output is one `uint` per cell: code, side, and one of four states. The states are *data*, *veil* (scanned but unproven, or an incompatible grid), *loading* (not scanned yet, or outside the row clip) and *no data* (outside availability). On screen: palette, grey veil, blue diagonal hatch in screen pixels, and nothing drawn.
- `HeatmapBinGrid`: absolute bucket and price-bin anchoring, with a 2-bin guard. A pan inside the guard changes only the draw mapping.
- `HeatmapRenderNode`: compute in `prepare()`, draw in `render()`. While a new timeframe uploads, the grid uses the active source's timeframe, so old columns keep their true time extent and are never stretched.

Findings the later slices need:
- **Exact codes need float-float, and Metal fast math breaks naive float-float.** Qt compiles MSL with fast math on, which folds two-sum error terms to zero. GLSL `precise` survives (SPIRV-Cross emits `[[clang::optnone]]` `spvFAdd`), but it doubles the kernel time. The kernel instead passes every intermediate through an XOR with a runtime zero. Evidence:

  | Kernel | Code mismatches, 10.2M real cells | Code mismatches, always-on stress test |
  |---|---|---|
  | float-only sums | 381 | not run |
  | float-float without the XOR | 381 | 9 |
  | shipped (XOR laundering) | 0 | 0 |

- **Display ticks must be multiples of `commonTick()`**, the LCM of a source's native ticks. The last deep day mixes $10 and $5 grids. A ladder tick of $25 would veil every $10 column as incompatible. S7's tick policy must use it.
- **Parity:**
  - Synthetic: 529k cells covering gaps, grid and size-scale changes, NotLoaded ranges, hour+minute levels, row clip and an incompatible tick. Stress test: 728k cells.
  - Real data, opt-in (`SENTINEL_HEATMAP_REAL_PARITY=1`, previous closed UTC day, deep 1m/5m/1h and near 1m): 10.2M cells.
  - All have zero state, code, side or validity mismatches against `bucketState` + `binColumn`.
- **Bench** (`sentinel-lab --bench`, base Apple M4 at 120 GB/s, last 24 h, 25 zoom levels × 200 compute passes per grid, GPU timestamps). Targets: p95 < 1 ms at 1x, p95 < 2 ms at 2x.

  | Case | Entries | GPU source | 1x p95 / max (ms) | 2x p95 / max (ms) | Result |
  |---|---|---|---|---|---|
  | deep 1m | 20.4M | 178 MB | 1.96 / 2.27 | 1.93 / 2.45 | **1x missed**, 2x met |
  | deep 5m | 4.1M | 36 MB | 0.56 / 0.67 | 0.52 / 0.59 | met |
  | deep 1h | 0.35M | 3 MB | 0.06 / 0.10 | 0.05 / 0.07 | met |
  | near 1m | 6.5M | 55 MB | 0.93 / 1.13 | 1.09 / 1.38 | met |

  - Why deep 1m misses: cost is linear in visible entries. The full-day zoom level reads 17.9M entries (143 MB) in 2.27 ms. The memory bandwidth floor for that level alone is about 1.2 ms, so p95 < 1 ms is physically out of reach whenever the view shows a full deep day at 1m.
  - The cost is per re-bin (tick, timeframe or source change, or leaving the guard), not per frame. A pan inside the guard does no compute.
  - Levels that show ≤ 5M entries run under 1 ms.
  - Options for the owner: accept about 2 ms re-bins at maximum zoom-out, or let the auto timeframe step up (1m → 5m) above a visible-entry budget.
- **Kernel changes tried and measured.**
  - Kept: compensated accumulation with one renormalization, the row index, and a log-estimated code corrected against the threshold table. Workgroup size (1×64 through 64×1) moves the time by only ±5%.
  - Rejected, slower: one thread per strip of 8 bins with walking cursors (3.7 ms), and an interpolation or galloping search (4.7 ms).
- **Lab:** `sentinel-lab` now hosts `HeatmapRenderNode` in a plain `QQuickItem`, which exercises the production path. This replaces `QQuickRhiItem`. `--screenshot` renders headless through `QQuickRenderControl`.
- **Deleted:** the lab `GpuBinner` and its shaders, `servermodel/RecordingEntries`, and their tests.
- **Review fixes (Codex gpt-6-sol review, same day):**
  - *D3D11 buffer size:* entries now live in up to 8 pages of ≤ 64 MiB. A native-tick group never straddles a page (the builder pads), so the kernel picks the page once per group. Every other source buffer is capped at 128 MiB. The deep day uses 3 pages. The first per-read page switch doubled the kernel time; choosing the page per group removed that cost (deep 1m 1x p95 2.02 ms, near 1m 0.88 ms).
  - *Allocation:* two grow-only buffer sets (active and spare) are reused across sources. At most one source is pending. A new capacity need creates at most one large buffer per upload step. A failed allocation or a refused source drops only the pending source; the active one keeps drawing. Re-requesting the active source cancels a pending upload (A → B → A).
  - *Re-bin key:* now includes the output size scale and the kernel variant, as well as the source id and grid coverage.
  - *Display tick:* the node picks it for the source it actually draws (`TickPolicy`), never for a still-pending one.
  - *Runtime precision self-test* (`HeatmapGpuSelfTest`), once per QRhi backend and device:
    - The fixture is 384 cells: 128 adversarial bins, whose exact sum lies 1e-9 above a code threshold while a plain float sum stays below it, plus random bins. It is compared with `binColumn`.
    - Until the test resolves, the binner uses the `precise` kernel variant. It switches to the fast kernel only if the test passes, and logs the result either way.
    - On this M4 the fast kernel passes. A folding kernel fails 128/384 cells and is rejected (test).
    - The precise fallback measures p95 3.22 ms against the fast kernel's 2.02 ms on deep 1m at 1x.
- **Second review round:**
  - *Refused sources are not retried every frame.* The binner remembers the refused source id and reports it once.
    - A source refused for limits or for the memory cap is never retried until a different source (or cap) arrives.
    - After an allocation failure it retries after a backoff of 2 s, doubling to 60 s.
  - *GPU memory cap.* Both source buffer sets together (active + spare) are capped at 512 MiB by default (`Frame::gpuMemoryCapBytes`, a constructor value). Before refusing, the binner releases unused spare pages. A refused source never disturbs the active one.
  - *Self-test fixture off the render thread.* The fixture (including the ~18 ms threshold table) is built once per process on a worker, started when the item or node is constructed. `prepare()` only records the self-test dispatch and readback, and only once an active source exists.
- **Unverified:**
  - The on-screen interactive lab window: the screen was locked, so no manual pan or zoom was done.
  - D3D11: HLSL 5.0 shaders compile (all three variants), but nothing has run.
  - Memory at 2x with several charts. The spare buffer set keeps the capacity of the largest earlier source (up to about 2× the source bytes).

- **Order:** S1 → (S2 ‖ S4 ‖ S5) → S3 (after L) → S6 → S7 → S8, then S9.
- **Parallel by files:** L with S1/S2/S4/S5; S2, S4 and S5 with each other.
- **Serial:** S3/L (SentinelStreamServer.cpp) and S6/S7/S8 (hot GUI files).
- Only one of these builds at a time.

## 5. Risks and open questions

1. **D3D11 (Windows build).** QRhi supports compute and read-only storage buffers there, but a single buffer is only guaranteed 128 MB. The GPU source must be paged into buffers of ≤ 128 MB, or budgeted to fit. The qsb HLSL 5.0 build of the compute shaders needs checking. Neither is verified; the owner has to run the readback test on Windows.
2. **Compute inside `QSGRenderNode::prepare()` is unverified.** The `beforeRendering` fallback is ready if it fails.
3. **Memory on a 16 GB Mac.** At 1 px/column, 1m tf and 3840 columns, deep is about 44M raw entries (about 265 MB). Row clipping plus time composition are what keep this in budget. Beyond the budget, zoom-out needs S9 or a clamp. The owner should choose the default budgets.
4. **Raw-only performance is unmeasured.** Re-benchmark before S6.
5. **HMC2 stores mean codes.** Composition is exact only in log-code steps (about 0.04% per code). Exact sums need the storage-pyramid format; the chunk codec version keeps room for that swap.
6. **Odd timeframes.** 90m is not an hour multiple, so it composes from 90 minutes per column (`buildPage` rejects it today). Near has no hour rollups, so there is no $1 detail at ≥ 1h.
7. **Multi-symbol and multiple charts.** Proposal: a process-wide chunk cache with a global budget, and one controller plus GPU set per chart.
8. **1s candles vs 1m heatmap.** Should the heatmap show 1m columns labelled "heatmap 1m", or hide below 1m until a 1s/100ms level exists? The owner needs to decide.
9. **WAN bandwidth for remote clients.** Unknown until S2 measures chunk sizes.
10. **Cold-start critical path.** TLS handshake + `server_config` + cold `Hmc2Reader` discovery + 1–2 deep hour chunks. The newest chunk must be requested first and painted on arrival.

### Critical files for implementation
- /Users/copeharder/Programming/Sentinel/libs/core/servermodel/RecordingPage.cpp
- /Users/copeharder/Programming/Sentinel/libs/gui/UnifiedGridRenderer.Render.cpp
- /Users/copeharder/Programming/Sentinel/libs/core/protocol/SentinelStreamServer.cpp
- /Users/copeharder/Programming/Sentinel/libs/gui/render/DataProcessor.cpp
- /Volumes/T7/sentinel-worktrees/lt-sol-gpu-bin-lab/libs/gui/lab/GpuBinner.cpp (plus `shaders/bin.comp`, `libs/core/servermodel/RecordingEntries.hpp`)
