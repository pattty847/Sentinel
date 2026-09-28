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

## Revision 2 (after Lt. Astra's review, 2026-09-28) — authoritative where it differs

Measured live (schema 3): deep ~1 KB/min deltas + ~35 KB keyframe per 15 min, near ~6 KB/min; ~15 MB/day, ~5.5 GB/year for BTC.

1. **Aggregate once, on the server, at the displayed tick.** The client computes its display tick from pixel geometry (square cells) and requests exactly that tick and row count; recording columns disable the client's secondary row grouping. Colour and labels both use the **summed TWAP quantity** of the dominant side at that tick (owner preference: walls add up). No server-sum-then-client-max.
2. **Transport fields:** keep `encoding: "base64"` (transport). Add `value_encoding: "absolute_log_size"`, `size_floor`, `codes_per_octave`, `source`, `layer`, `request_id`, `band_generation`, and per-column validity (see 3). Errors echo `request_id`.
3. **Coverage is explicit.** Each column carries a row-validity bitmask (1 = recorded, 0 = unknown) derived from the minute's side bounds; an aggregate row is valid only if all constituent rows were covered for the time used. Pages carry `scanned_start/scanned_end`, `next_end`, `exhausted`; a short page never marks older history missing, only the scanned interval.
4. **Bounded, streaming reads.** `Hmc2Store` gains a streaming reader (visit reconstructed records in order without retaining them), per-request budgets (source records, entries visited, wall-clock ms), a stop token, and a cached per-series availability (oldest/latest bucket). Page size shrinks to fit the budget; the reply says so (not exhausted).
5. **Hourly records carry coverage.** Hour (tf=3600000) records store per-entry covered-ms (new schema for hour records), so multi-hour rollups are exact: `sum(size*coveredMs)/sum(coveredMs)`. Output tf >= 1h reads hours (plus the current hour's tail from minutes, no double count); tf < 1h reads minutes. Persisted 15m/4h only if measurement demands it.
6. **Cells are half-open on the absolute grid:** row k covers [k*tick, (k+1)*tick); price of a row is its lower edge; band rows = ceil(hi/tick) - floor(lo/tick) <= rows (grow the tick one 1-2-5 step if not). Wire order stays descending from the top row. Client grouping phase and label coordinates are corrected to the lower-edge convention together.
7. **Dominant side, single channel** for v1, with deterministic ties (bid on tie); bid and ask accumulators are kept until final projection. Labels state dominant-side quantity.
8. **Unknown stays unknown** outside near coverage (no expanding $10 totals into $1 rows).
9. **Client:** `ColumnWindow::setDisplayBand()` re-bands without `configure()/clear()`, keeps live cache/placement/slots (INV-045), rewrites all slots and validity; projected cache keyed by (source, tf, band_generation); obsolete replies dropped by request_id; viewport dedup includes price bounds; legacy live ingestion gated off in recording mode. Shader gets a value-mode flag and a validity-aware path.

Slices: S1 (builder + streaming reader + budgets + availability + hour-record coverage, core, tests) -> S2 (protocol + server wiring + client DTO parsing) -> S3 (client re-band + shader + labels; orchestrator) -> S4 (live per-client + provisional minute; needs a recorder publication API).


## S1 implementation and measurements (Lt. Astra, 2026-09-28)

Core entry points are `Hmc2Reader` in `Hmc2Store.hpp` and `recording::buildPage` in `RecordingPage.hpp`. The API, budget units, paging guarantees and schema 4 byte layout are documented in `docs/MARKETDATA.md`. Protocol/client integration remains S2/S3.

Two clarifications to Revision 2 were necessary:

- Per-entry covered-ms alone is insufficient for exact rollups of sparse hours: an absent row can be either a covered zero or a coverage hole. Schema 4 also stores disjoint per-side coverage runs, including zero rows. Schemas 1-3 hours use observedMs and propagate an approximate-coverage flag. Exactness is with respect to stored quantized TWAPs, not lossless original sizes.
- This recorder persists deep hours only. Hour-or-coarser requests select deep and reject sub-deep display ticks; they never expand deep quantities into near cells. Adding near hourly persistence would be a separate capability change.

Rough measurements on the configured mac-clang arm64 build (`CMAKE_BUILD_TYPE` and CXX flags empty; **unoptimized**, not release), local synthetic files. Deep uses 12,000 entries/record; near uses 1,200. Prices are projected into at most 2,048 rows. Fixture construction/fsync time is excluded. These are observations, not CI thresholds:

| Request | Columns | First reader pass | Repeated pass |
| --- | ---: | ---: | ---: |
| Deep 1m, synthetic day | 1,024 | 915 ms | 903 ms |
| Near 1m, synthetic day | 1,024 | 457 ms | 540 ms |
| Deep 4h, one day of hours | 6 | 160 ms | 91 ms |
| Deep 4h, 4,096 hourly sources | 1,024 | 16,325 ms | — |

The full 4h page performed 74.8M charged entry-work units (including coverage/reconstruction), far less than reading 245,760 minute sources but still expensive in this build. With default 5,000 ms request budget, it returned 317 completed columns in 5,008 ms, status Budget, exhausted=false. Deadlines are cooperative; one bounded frame/projection can overrun slightly. Keep short-page budget semantics in S2; do not assume that using hours alone makes a 1,024-column 4h page cheap. Measure an optimized build before deciding whether persisted 4h rollups are justified.

Reproduce the optional measurements with `build/mac-clang/tests/servermodel/test_recording_page --gtest_also_run_disabled_tests --gtest_filter='*Timing'`. Ordinary CTest excludes these two fixtures and has no timing assertions.


## S1 review follow-up (2026-09-28)

Reader candidates now check frame-index bounds after refresh and verify both decoded and selected bucket identity. Selected-frame open/read failures report IoError without extending the scanned interval; content corruption still falls back to earlier valid generations. Discovery skips and warns about denied/corrupt files and retries incomplete availability on later requests.

Index discovery retains its partial cursor across budget stops. Append-only growth validates the old tail frame and scans only new frames; truncation, rewrite or tail damage rebuilds. Metadata eviction is LRU, and directory listings are cached by directory mtime rather than enumerated once per output column. Debug builds assert worker-thread ownership.

Budgets are soft admission limits with minimums of 1 source record, 1 entry-work unit and 10 ms. The first selected record/chain is allowed to exceed them; counters expose that work. Cancellation is not deferred. Very small cold requests can first return no columns while advancing cached discovery, so S2 must reuse its per-worker reader. A large rollup still needs a budget sufficient for one whole output bucket; partial buckets never become proven history.

Hour rollups are exact relative to the **stored quantized values**. BookRecorder re-encodes each hour TWAP, so an hour-based rollup and a rollup directly from minutes can differ by roughly one size-code step from rounding; coverage precision does not make the codec lossless.
