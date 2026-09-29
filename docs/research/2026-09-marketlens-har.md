# MarketLens HAR analysis (2026-09-29)

Source: `~/Downloads/marketlens.app.har` (511 entries, ~24.6 s; the owner refreshed, zoomed in, zoomed out). t = seconds after the first request (20:58:42 UTC). `#n` = entry index. No header, cookie or credential values are reproduced. Tooling: python3 stdlib, plus `strings` on the captured WASM and JS bundles.

## 1. Endpoints and request keys (OBSERVED)

All data is `GET api.marketlens.app/markets/<exchange:market>/data/<kind>?v=5&dt=<ms>&start=<epoch ms>[&dp=<price step>]`, one request per market per chunk. Six BTC markets load in parallel (binance futures/spot, bybit futures/spot, coinbase, hyperliquid).

| kind | params | chunk span (1m) | grid alignment |
|---|---|---|---|
| `orderbooks` (heatmap) | dt, start, dp | 64 min = 3,840,000 ms | `start % 3,840,000 == 0` (epoch) |
| `trades`, `footprints` | dt, start, dp | 128 min | `start % 7,680,000 == 0` |
| `basics` (candles/stats) | dt, start | 512 min | `start % 30,720,000 == 0` |

- `dt=60000` and `dp=10.0` on all 252 data GETs. **No request changes resolution during zoom.**
- The chunk spans are fixed column counts (64/128/512 columns). They are powers of two and aligned to the Unix epoch, not to the session or the viewport.
- The client also requests future chunks. Those return an empty body: 5 B for `basics` (start 01:04 next day), 8 B for the `trades` and `footprints` chunk that starts at 20:48.
- JS bundle: `dt` is the user's timeframe (`1s,5s,15s,1m,1h,4h`). `dp` and `heatmapDp` are per-timeframe user settings. Neither value depends on zoom.
- Other traffic: `market_groups/BTC-USD` JSON (102 KB), `api.hyperliquid.xyz/info` metadata POSTs, analytics. None of it is heatmap data.

## 2. Payload formats (OBSERVED)

- Data responses are `application/octet-stream` with `content-encoding: zstd`. After zstd, the orderbooks, trades and footprints bodies start with the magic `pco!`. This is **pcodec** (the WASM embeds `pco-1.0.2`), one pco file per response. I did not decode pco, because stdlib cannot. Dense versus sparse layout on the wire is therefore **unknown**.
- zstd adds almost nothing on top of pco. #244: 50,568 B decoded, 51,243 B transferred (headers included). Wire size is known only for the 6 network-served responses.
- `basics` is not pco. It is a flat columnar layout: a small header, a u64 start time, u32 ms offsets (step 60,000; 204 rows in a partial chunk, 512 in a full one), then f64 columns. For bnS/byS/cb the size is exactly 196 B/row (4 + 24 x f64). The same row count gives the same size across those markets, so the layout is dense and fixed-width.
- Decoded orderbook chunk size per 64-min chunk: bnF 186-237 KB, cb 169-192 KB, bnS 125-153 KB, hl 117-135 KB, byS 28-40 KB, byF 16-17 KB. There are 108 unique chunks in total, 5.49 MB (orderbooks 4.05 MB, basics 1.13 MB).
- WASM crates: `pco`, `bitcode`, `gloo-net`, `rhai` (script indicators). Source files: `src/loader.rs`, `src/datasource/heatmap.rs`, `src/data/heatmap.rs`. The fetch initiator stack for every data request is `window.fetch <- __wbg_fetch <- wasm <- requestAnimationFrame`. **The WASM render loop decides which chunks to fetch.**
- GPU (JS): the heatmap is a WebGL2 `TEXTURE_2D_ARRAY` of **256x256 `R32F` tiles**. It starts at 16 layers and grows by reallocating, which copies the old layers back through `readPixels`. Each frame, `wasm.heatmap(yDomain, units, ...)` returns `{zRange, modified, uniforms, buffers}`. JS uploads only the `modified` tiles (`texSubImage3D`) and draws one instanced quad per tile. Pan and zoom are `translate`/`scale` uniforms. Cell values are signed (bid > 0, ask < 0, NaN = no data). The fragment shader applies z_min/z_max, a log or linear colour map, and cell-gap lines when a cell is 5 px or wider.

## 3. Sequencing and caching (OBSERVED)

| t (s) | Event |
|---|---|
| 2.5 | HTML (304) |
| 4.0-4.4 | 3 WebSockets open, including `/markets/stream?dp=10.0&dt=60000&update_dt=100&v=5` |
| 5.29-5.50 | WS snapshot for each market (0x00 frame, gzip, 116-314 KB wire, 0.2-0.46 MB decoded, 3 pco blocks each). Header start = 1790711040000 = **19:44**, the start of the *previous* 64-min chunk |
| 5.52 | Live 0x01 updates start (~10/s per market) |
| 5.94 | REST: orderbooks 18:40 (the chunk just before the snapshot start) |
| 6.22 | orderbooks 17:36, 16:32, 15:28; trades/footprints 16:32, 14:24 |
| 6.92 | basics 16:32 and 01:04 (future); orderbooks 19:44 and 20:48; trades/footprints 18:40 and 20:48 |
| 8.5-8.9, 10.6-10.9, 12.6-13.0 | Three bursts. Each re-requests orderbooks 18:40 to 14:24 (14:24 is new in the first burst), trades 14:24, and basics 08:00 (new in the first burst) |

The capture ends at 24.6 s. The loader fetches newest to oldest, in parallel across markets. Client-side queueing (`_blocked_queueing`) is 110-320 ms per request.

Caching:
- **246 of 252 data responses came from the browser HTTP cache.** Their `Date` header is 19:55 (one hour before the capture), server wait is 0.3-2 ms, and transfer size is 0. Only the 6 `orderbooks` 20:48 chunks went to the network (Date 20:58:49, 288-430 ms).
- Headers: `Cache-Control: max-age=14400` (4 h) on the open chunk and the one before it. `max-age=604800` (7 d) on older chunks. `Last-Modified` and `Vary` are present. There is **no ETag and no `immutable`**. `cf-cache-status` HIT is on the 7-day chunks only.
- **Stale data from the cache.** The open chunks cached at 19:55 are reused at 20:58. #243 (orderbooks 19:44) is 60.7 KB against ~186 KB for a full bnF chunk; its size fits about 11 min of data. The 20:48 trades and footprints chunks are the 8 B empty bodies cached when 20:48 was in the future. The client adds no cache-busting parameter.
- Repeat fetches: 144 of 252 GETs ask for a chunk that the page already fetched (orderbooks 84/126, trades 24/48, footprints 24/48, basics 12/30). The browser cache served them in 1-12 ms.

## 4. WebSocket (OBSERVED)

- `/markets/stream`: binary, gzip per frame. The first frame is the subscription (a list of market ids). The client then sends a 10-byte frame each 1 s that holds an f64 counter, and the server replies with type-3 frames (heartbeat).
- Frame kinds: 0x00 = history snapshot from the previous chunk boundary to now (pco). 0x01 = live update every ~100 ms (`update_dt=100`). The only minute stamps in the updates are the current columns (20:58: 1503 hits, 20:59: 694). Updates are 26 B-6.6 KB decoded; all updates together total 669 KB wire over 19 s for 6 markets (~35 KB/s).
- Two more sockets (`.../quotes/USD/stream`, `.../BTC-USD/markets/stream`) carry gzip JSON ticker and stats snapshots and updates at ~1/s. They carry no heatmap data.

## 5. INFERRED, and what it means for Sentinel

These points are inferences, not observations.

1. **Fixed-resolution server, client-side aggregation.** The server never re-bins for zoom. It ships dt x dp columns at the user-selected resolution, and the WASM builds render-ready 256x256 float tiles. The GPU only maps values to colours. This is "whole chunk -> CPU aggregation into cached tiles", not viewport-clipped GPU binning. I could not tell whether the WASM rebuilds tiles when the y-domain changes on zoom-out, because `heatmap(yDomain, ...)` receives the domain every frame.
2. **Live edge = snapshot, history = REST.** The WS snapshot covers [previous chunk start, now] and so hides the stale open-chunk entries in the HTTP cache. REST effectively serves sealed history only, fetched newest first. Sentinel's plan item 10 (newest chunk first, painted on arrival) matches this. Sentinel's "cache sealed chunks only" rule is correct, and MarketLens shows the failure when a system breaks it: a 4 h max-age on open chunks, and cached empty future chunks.
3. **Eviction.** Each viewport change re-requests every visible chunk. The loader seems to keep no decoded-chunk cache of its own and to use the browser HTTP cache as its L2. The 1-12 ms hits hide the cost in a browser. For Sentinel, keep the planned decoded-RAM LRU in front of the disk cache, so that a zoom does not re-read and re-decode chunks.
4. **Chunk size.** 64 columns x one market is 16-237 KB after pco. Small chunks at a fixed grid give many parallel, cheap requests: 126 orderbook GETs in about 7 s. Sentinel's hour chunks at 1m (60 columns) are in the same range. A power-of-two column count (64) with epoch alignment makes the chunk-id math trivial at every tf.
5. **Prefetch.** Initial load: orderbooks from 15:28 (~5.5 h), trades from 14:24 (~6.6 h), basics from 16:32. The first post-load burst added one older chunk per kind (orderbooks 14:24, basics 08:00). The margin looks like about one chunk beyond the visible range, not the "+-1 viewport" that the Sentinel plan uses.
6. **Codec.** Integer/float column codecs (pco) make transport compression redundant. If Sentinel's chunk codec is already entropy-coded, do not also zstd the payload.
7. **GPU layout.** A signed single-channel float tile array (bid +, ask -, NaN = empty) with dirty-tile uploads is a cheap render-ready format. The weak point is growth, which reallocates through a `readPixels` round trip. Sentinel should preallocate to a budget instead.

Not verified: pco payload layout (dense grid or sparse levels, number of price levels per column); what the 3 pco blocks in the snapshot hold (possibly orderbooks, trades and footprints); which burst was zoom-in and which was zoom-out (three bursts, two gestures); the wire size of the cache-served chunks.
