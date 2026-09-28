# Recording v2 serving: from HMC2 to the chart

Status: design, 2026-09-28. Author: orchestrator. Review: Lt. Astra (read-only).
Depends on: docs/research/2026-09-recording-v2.md (recorder + HMC2, live since 2026-09-28 02:43).

## Goal

Zoomed out, the chart shows the deep layer's whale lines across the whole price range; zoomed in, near-layer $1 detail. Rows are always aggregated to the display's price tick on the server, so the client never receives more rows than it draws.

## Facts this design builds on

- HMC2 records per (symbol, layer, tf): `(row, side) -> twapCode, peakCode` on a fixed log size scale; near = $1 rows within +/-5% of the mid, deep = $10 rows over [mid/4, mid*4]; 1m records, deep also 1h. Schema 3 stores deltas with 15-minute keyframes.
- The client's `heatmap_window::ColumnWindow` already keeps per-column source bands and resamples each column into one display band (`resampleColumn`); a band change rewrites every slot (`Update::full`).
- The client texture is R16 `code|sideBit` per cell; today's value is a per-column log-normalized intensity. Labels read a separate linear liquidity channel (`u16 * liquidityScale`).
- The display tick groups rows client-side (HeatmapRowGrouping, square-ish cells).

## Design

### 1. Server: recording column builder (core, pure, testable)

`recording::buildColumns(root, symbol, tfMs, endMs, count, priceLo, priceHi, rows) -> std::vector<ServedColumn>`

- Output row tick = smallest `layerTick * {1,2,5,10,...}` with `tick * rows >= priceHi - priceLo`. Band = `[floor(priceLo / tick) * tick, + rows * tick)`, on the absolute tick grid.
- Layer: near when the output tick is below the deep tick, else deep. Inside a near column, rows outside that minute's recorded bounds are unknown (0), not zero liquidity; a per-column `coveredLo/coveredHi` travels with the column.
- Aggregation per output row, per side: **sum** of decoded TWAP sizes (quantity semantics). Peak per row = max of decoded peaks.
- Cell value: dominant side (larger summed size) encoded as `encodeSize(sum) | sideBit` with the recording's size scale: an **absolute log code**, not a normalized intensity. A second channel carries the dominant side's summed size linearly (`u16 * scale`, scale = column max / 65535) for labels, as today.
- Time: tf = 1m reads 1m records. tf = k * 1m (5m, 15m, 4h) rolls up 1m records at serve time: per (row, side) `sum(size * observedMs) / sum(observedMs of covering minutes)`, peak = max, bounds = intersection (rows outside the intersection are unknown). Deep tf >= 1h reads the persisted 1h records (1d rolls up from 1h). Never average codes.
- Paging: `count <= 1024`, `endMs` bucket start as today; returns `oldestAvailableMs` for the series.
- Cost bound: one page = up to 1024 columns x (entries per record) decoded + aggregated. Deep ~12k entries/record: ~12M entry visits per 1024-column page, a few hundred ms; runs on the existing history worker pool, never on the network thread.

### 2. Protocol

- `heatmap_history_request` gains optional `price_min`, `price_max`, `rows`, `source` (`"recording"` | `"legacy"`, default legacy). With `source: "recording"` the response columns are self-describing as today (band per column) plus `encoding: "logcode"`, `size_floor`, `codes_per_octave`, `layer`, `covered_min`, `covered_max`.
- Server config advertises `recording.available` and the recorded symbols/layers/timeframes so the client greys out what cannot be served (as served timeframes do today).

### 3. Client

- **Band follows the view.** DataProcessor keeps a display band = the visible price range with 50% margin on each side, at the output tick chosen for the texture's row count (2048). When the view leaves the margin, or the ideal tick changes by a 1-2-5 step, it requests a re-band: the window reconfigures its band (`full` rewrite) and refetches the visible pages with the new `price_min/price_max/rows`. Debounced (~150 ms) during zoom so a wheel gesture fetches once.
- **Shader logcode mode.** A uniform flag selects the value meaning. In logcode mode, magnitude = `clamp((code - lo) / (hi - lo), 0, 1)` with `lo/hi` uniforms (the sensitivity range; default from config, e.g. 0.01 BTC .. 100 BTC). The existing gamma/contrast/palette stay. Max-by-row grouping still works (codes are monotonic in size).
- **Labels** unchanged: they read the linear channel and sum within display groups.
- Config `heatmap.source: recording | legacy` (default legacy until verified), so the change can be A/B tested and rolled back.

### 4. Live (second slice)

- The recorder publishes each closed minute (a callback from the worker, queued to the model thread) and, every ~1 s, a provisional snapshot of the open minute (integral so far / valid time so far). Cost: O(rows touched), ~12k entries per second per layer.
- Each client tells the server its current view band (`heatmap_view` message: price_min, price_max, rows, tf). On each provisional or closed minute, the server builds that client's column with the same builder (one column, cheap) and sends it as a live slice with `encoding: "logcode"`.
- Until slice 4 lands, recording mode shows history only up to the last closed minute plus the legacy live column is not mixed in (different value meaning).

## Build order

1. Builder + tests (core; synthetic HMC2 fixtures: layer choice, tick choice, sums vs peaks, dominant side, near coverage, rollups with partial coverage, deep 1h, paging). Delegate.
2. Protocol + server wiring into the history worker pool + config advertising. Delegate (same agent).
3. Client re-band + shader logcode mode + config switch. Orchestrator (render hot path), verified with the Agent API at several zooms.
4. Live per-client columns + provisional minute. Delegate, orchestrator reviews threading.

## Open questions for review

1. Dominant side per cell vs two channels (bid and ask sizes) at the cost of 2x bytes and a new texture layout. At the spread both sides can be present in one output row; dominant side hides the smaller one.
2. Is sum the right colour quantity when zoomed out (a $1,000 row sums many levels), or should colour use the max row-level size and labels the sum? (Owner saw sum as right for walls; max keeps single whales crisp in wide rows.)
3. Serve-time rollups for 5m/15m/4h from 1m: cost for a 1024-column 4h page is 1024 * 240 minutes of deep records (~3 billion entry visits). Persist 15m/4h deep rollups too, or cap and page smaller?
4. Unknown vs zero outside near coverage when mixing layers in one view: fall back to deep rows for those cells (coarser) instead of unknown?
