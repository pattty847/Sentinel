# Recording v2: near and deep order-book layers

Status: design, 2026-09-28. Author: orchestrator. Review: Lt. Astra (read-only), then build.

## Why

- Today the heatmap records 2048 rows x $5 around the mid (about +/-6% for BTC). Walls further out are never stored.
- The server receives the whole Coinbase book (about 42k levels for BTC-USD) but `ServerDataModel::computeBandRange` clips the live book to +/-30% at $0.10, and the heatmap samples a smaller band of that.
- Stored values are log-normalized per column against a running max (`HeatmapTwapStreamer` ~line 900). A 120 BTC wall in March and one in September cannot be compared. The separate `liquidity` column is linear but scaled per column (`u16 * liquidityScale`).
- The owner's target (TapeSurf screenshots, memory `tapesurf-inspiration`) shows about $25k-$144k over 19 days: long-lived whale walls far from the price are the point.
- Coinbase does not serve past order books. Every day not recorded is lost, so the write side ships first, before any display work.

## Decisions (owner-approved 2026-09-28)

| Layer | Price range | Row tick (BTC) | Columns | Purpose |
|---|---|---|---|---|
| Near | +/-5% around the mid (follows the mid) | $1 | 1m | order-flow detail, in-cell text zoom |
| Deep | whole book clipped to [mid/4, mid*4] | $10 | 1m, plus persisted 1h rollup | whales over weeks and months |

- Per-asset ticks: near tick is about 1 bp rounded down to 1-2-5 (owner chose $1 for BTC); deep tick = 10 x near tick.
- Store absolute resting size per price row with one fixed scale forever. Colour normalization moves to display time (a two-handle sensitivity range).
- Store only occupied rows. Compress.
- The existing HMCL `.hmcol` store and today's heatmap path keep running unchanged until the display side moves to v2.

## Write path

New core class `BookRecorder` (libs/core/servermodel), owned by `ServerDataModel`, one instance per symbol.

Input (raw, before any band clip):
- `onLiveOrderBookInitialized(bids, asks)`: full snapshot. Resets state and marks the current column as resynced.
- `onLiveOrderBookLevelUpdates(updates, exchangeMs)`: absolute level sizes (`BookLevelUpdate{isBid, price, quantity}`).

State:
- Full book map: price (integer, in the exchange's quote increment) -> size, per side. About 42k entries for BTC.
- For each layer, per occupied row: current size (sum of levels in the row), `lastChangeMs`, `accumSizeMs` (size x ms) for the open column, `maxSize` for the open column (optional, see open questions).

Time weighting is event-driven and exact, not sampled:
- On each level change: row = floor(price / tick). `accum += currentRowSize * (t - lastChangeMs)`; update `currentRowSize` by (new - old level size); `lastChangeMs = t`.
- At column close (minute boundary by exchange time, driven by the existing 50 ms timer or the next event): for every row with `currentRowSize > 0` or `accum > 0`, finish `accum += currentRowSize * (close - lastChangeMs)`, emit `twap = accum / observedMs`, reset `accum`, set `lastChangeMs = close`.
- `observedMs` excludes time without a valid book (disconnect, before the snapshot). Never fabricate (FM-044).
- Side per row: rows at or below the column's closing best bid are bid rows, at or above the best ask are ask rows. A row that held both sides during the minute stores two entries (one per side).
- Near inclusion window: rows within [minMid * 0.95, maxMid * 1.05] over the minute. Deep window: [mid/4, mid*4] at close.

Cost: per book update O(1) hash work per layer. Column close is O(occupied rows), about 10-40k once per minute. No per-50 ms scan of the book.

## Size encoding (fixed log scale)

`code = clamp(round(log2(size / kSizeFloor) * kCodesPerOctave) + 1, 1, 32767)`, 0 = empty.
- `kSizeFloor` per asset in base units (BTC: 1e-6). `kCodesPerOctave = 819` gives 40 octaves (1e-6 .. 1e6 BTC) in 15 bits at about 0.085% relative precision.
- Bit 15 = side (1 = ask). This keeps the existing bid/ask-by-high-bit convention used by `intensityMagnitude`.
- Decode: `size = kSizeFloor * 2^((code - 1) / kCodesPerOctave)`. Notional (USD) is computed at display/API time from the row price.

## File format (HMC2)

Per symbol, per layer, per UTC day: `<dir>/<symbol>/<layer>/<YYYY-MM-DD>.hmc2`.
- File header: magic `HMC2`, version, symbol, layer id, tick size, kSizeFloor, kCodesPerOctave, timeframe ms.
- Append-only records, one per closed column: `[u32 magic][u32 payloadLen][u32 crc32][payload]`.
- Payload (then zstd-compressed as one frame per record):
  - `i64 bucketStartMs`, `u32 observedMs`, `u8 flags` (resynced, partial), `f64 midOpen, midClose, bestBidClose, bestAskClose`
  - `u32 entryCount`, then entries sorted by (row, side): `varint rowDelta` (row index relative to the previous entry; first entry absolute as i64 varint), `u16 code|side`.
- Recovery: on open, scan records, stop at the first bad CRC, truncate the tail (same approach as `HeatmapColumnStore`). flock per directory.
- Deep 1h rollup: written by the recorder at each hour close from its own 1m columns (time-weighted mean of sizes over observed minutes), same format with timeframe 3600000. 1d can be rolled up at serve time from 1h.

Estimated size (BTC, compressed): deep 1m about 15-25 MB/day, near 1m about 10-20 MB/day, 1h negligible. About 10-20 GB/year per symbol. `persistence_dir` points at the T7 (`/Volumes/T7/sentinel-data`), with a fallback when the drive is not mounted.

## Read and serve path (step 3, after the recorder ships)

- Request: symbol, layer or auto, time range, timeframe, price range, row count.
- Server picks the layer (deep when the requested rows are coarser than the deep tick), merges rows into the requested row tick with max-by-size per side, and returns dense rows of codes for the price window. Encoding stays code|side, so the client's texture holds absolute log sizes and the shader maps a [lo, hi] code range to the palette (the sensitivity slider).
- Time: 1m for near; deep 1m or 1h, depending on the requested timeframe.

## Build order

1. `BookRecorder` + HMC2 writer + reader + tests (synthetic book event streams with known TWAPs, gaps, resync, side flips, CRC tail recovery, round-trip of codes). Wire into `ServerDataModel`, config keys under `recording:` (enabled, dir, near_pct, near_tick, deep_tick_mult, deep_min_frac, deep_max_mult, size_floor). Ship: it records to disk, nothing reads it yet.
2. Serve path + client LOD requests + display (separate steps).

## Open questions for review

1. TWAP only, or TWAP plus the max size in the minute (catches large orders that flash in and out; +2 bytes per entry)?
2. Side assignment by the closing best bid/ask vs tracking side per level through the minute.
3. zstd one frame per record vs a block per N records (better ratio, but a crash loses the open block).
4. Should the recorder run on its own thread (it touches every book update) or on the model thread like today?
5. Any risk from the exchange-time clock (out-of-order updates, clock skew vs `exchangeNowMs`)?
6. Is 1h enough as the only persisted deep rollup, or also 15m?

## Revision 2 (after Lt. Astra's review, 2026-09-28) — authoritative where it differs from the text above

Review: raw L2 callbacks are unclipped absolute sizes (MarketDataCoreEngine.cpp:421/469); clipping happens in LiveOrderBook. Hooks stay.

1. **Book validity.** The engine emits ordered validity events: invalid on disconnect, sequence gap, malformed L2; valid again only at the next snapshot. The recorder integrates only valid intervals. `observedMs` counts valid time only; an invalid interval keeps the minute's already-accumulated valid numerator (levels and integration timestamps reset, accumulators do not). Zero observation = unknown, never zero liquidity. (Orchestrator implements the engine side.)
2. **Time.** Integration time = the L2 message envelope timestamp (ms), made non-decreasing per symbol (a backward step is applied at the current clock and counted). Snapshots carry their envelope time; observation starts there. Integration is split at every crossed minute boundary before applying a batch. Columns close only when the recorder clock passes `bucketEnd + lateness` (default 2 s); an idle book is advanced by a timer using local time minus the last seen (local - envelope) offset. Events older than an already-closed bucket are applied at the clock time and counted as late.
3. **Side is tracked per (row, side)** for the whole minute: separate current size, integral and peak. Both sides may occupy one row; a record then holds two entries for that row (the second with row delta 0). Rows with zero current size are kept until their contribution is emitted.
4. **Accumulate every occupied row, filter at close.** The near window is [minMid*0.95, maxMid*1.05] over the valid part of the minute; the deep window is [minMid/4, maxMid*4]. The record stores the actual included row bounds per side; inside the bounds, absent means zero; outside, unknown.
5. **TWAP plus peak** per (row, side). One message's levels are applied atomically before peaks are taken, so application order cannot create false peaks.
6. **HMC2 identity and schema.** Path: `<root>/<symbol>/<layer>-<tfMs>/<YYYY-MM-DD>.hmc2`, file chosen by bucket start (UTC). Little-endian. File header: magic, schema version, header length, header CRC, symbol, layer, timeframe, price scale (units per quote increment), row tick in price units, size floor, codes per octave, config hash; a mismatching config opens a new file generation instead of appending. Record: `[u32 magic][u32 compressedLen][u32 rawLen][u32 crc32 of compressed bytes][zstd frame]`, rawLen bounded (reject > 16 MiB). Raw payload: `i64 bucketStartMs, u32 observedMs, u32 flags (partial, resynced, late events, underflow), i64 bidRowLo, bidRowHi, askRowLo, askRowHi, f64 midOpen, midClose, midMin, midMax, u32 entryCount`, then entries sorted by (row, side): `varint zigzag rowDelta` (first entry relative to 0), `u16 twapCode|side`, `u16 peakCode`.
7. **Recovery.** Auto-truncate only an incomplete terminal record (length runs past EOF). A bad CRC or magic in the interior is skipped and logged (resync by scanning for the next magic); the file is never truncated for interior damage. Readers take the last record per bucket (restart dedup). fsync each record and new directories; surface disk errors. The open minute is lost on crash (documented). One lock file at the recorder root. Reuse the CRC and locking helpers in HeatmapColumnStore.cpp.
8. **Prices.** price -> integer units with checked nearest rounding (`llround(price * priceScale)`, reject non-finite, non-positive, overflow), then `floorDiv(units, rowTickUnits)`. Never floor a floating quotient. Sizes: reject non-finite or negative; sizes below the floor encode as 1 and set the underflow flag.
9. **Hourly rollup** (deep only): per (row, side) `sum(twap * observedMs over minutes whose bounds cover the row) / sum(observedMs of those minutes)`, peak = max. Decode sizes before averaging; never average codes. Rebuilt on restart from committed 1m records of the current hour.
10. **Threading.** A dedicated recorder worker thread owns all recorder state. The model thread only enqueues batches (bounded; on overflow the recorder marks the symbol invalid until the next snapshot and counts the drop). Encoding, compression and fsync happen on the worker.
11. **Serving (later step):** price aggregation uses sums for quantity; max is exposed separately as a display statistic.
12. Storage figures are estimates until measured on a live capture.
