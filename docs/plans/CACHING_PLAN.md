# Caching Plan (Heatmap + Candles)

Last updated: 2026-01-29

## Goals
- Keep server authoritative for heatmap/candle history.
- Make client redraws instant for visual tuning (gamma/contrast/floor, labels, thresholds).
- Avoid re-requesting data during UI tweaks.
- Support fast symbol switching with cached replay.
- Keep GPU textures as disposable views; cache lives on CPU.

## Decisions (Authoritative)

### 1) Server Heatmap Caching
- Server is the sole producer and historian of heatmap columns.
- Persisted anchor timeframes per symbol:
  - 1s, 1m, 1h, 1d
- Each anchor timeframe has a bounded FIFO ring buffer (~15k columns target).
- Columns store raw per-price-bucket values (liquidity/intensity), not visual-mapped values.
- Grid definition (tick size, height, min/max, width) is authoritative from server.
- Grid width (history columns) is decoupled from grid height (price buckets).
- Grid definition changes invalidate cached columns for that stream.

**Derivation (server-side):**
- Non-anchor timeframes derived on request by grouping nearest lower anchor:
  - <1m from 1s
  - <1h from 1m
  - <1d from 1h
- Derived TFs are not cached server-side.

**Protocol:**
- On subscribe, server sends a history window sized to `gridWidth` columns, ending at latest.
- Server streams live columns at active TF.
- Server supports historical range requests within cached bounds.
- Out-of-range requests return out-of-range.

### 2) Client Heatmap Caching
- Per `(symbol, heatmapTF, gridDefinition)` in-memory ring buffer.
- Target size: `gridWidth` columns default (configurable).
- In-memory only; cleared on exit.
- Client cache is for replay/redraw only (not truth).

**Rules:**
- Client never invents or re-buckets data.
- Client never mutates cached columns; only re-renders.
- GPU textures are disposable views derived from cache.

**Scrolling behavior:**
- If user scrolls beyond client cache:
  - Client requests older columns from server.
  - Server responds from anchor cache (or derived rollup) in `gridWidth` chunks.
- If server cannot satisfy request, scrolling stops.

### 3) Labels / Text Rendering
- Deterministic regeneration, no geometry cache (v1).
- Labels are regenerated from cached columns + current settings.
- No server involvement.

### 4) Candles (OHLCV)
- Server caches OHLCV per TF (existing TimeframeAggregator).
- Server may roll up higher TFs from lower ones.
- Client caches candle bars per `(symbol, TF)` for instant redraw/switch.
- Same scroll-past-cache behavior as heatmaps.

### 5) Invariants
- Server is the only source of heatmap columns.
- Client cache exists for replay/redraw, not truth.
- Grid definition changes invalidate all related caches.
- No arbitrary TF persistence; anchors + derivation only.

## Clarifications / Follow-ups
- Grid width (history columns) is independent of monitor resolution; GPU scales the texture.
- Suggested default width: 4096 columns (TV-like), with option to move to 5120 later.
- Client eviction policy should prefer “farthest from current view” over pure FIFO.
- Align derived TF buckets to anchor start time to avoid drift.
- Define a `GridKey` for cache invalidation:
  - width, height, tick size, min/max, time origin, timeframe.

## Open Questions
- Default `gridWidth`: 4096 vs 5120? (history depth vs memory)
- Max memory budget for per-symbol caches (LRU eviction)?
- Should we expose cache size as user setting or env var?

## Next Steps (tomorrow)
- Map cache responsibilities to existing classes:
  - `HeatmapStreamState`, `DataProcessor`, `RemoteGridDataSource`.
- Propose concrete data structures for server and client ring buffers.
- Define history request API shape and grid invalidation events.
  - request: `(symbol, tf, gridKey, endTime, count)`
  - response: `history_chunk` with metadata + columns (count <= gridWidth)
