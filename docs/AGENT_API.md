# Agent API (v1)

The GUI listens on `127.0.0.1` at `gui.api_port` (default `17100`). `api_port=0` disables it. This is a local observation API; reads do not fetch history. All times are UTC epoch milliseconds. It currently exposes these routes:

| Method | Route | Result |
|---|---|---|
| GET | `/api/v1/state` | Connection, advertised server configuration, active layers and per-source last receive times. |
| GET | `/api/v1/viewport` | Active chart bounds, linked heatmap/candle timeframe, follow mode, dimensions, zoom and viewport version. |
| GET | `/api/v1/candles?startMs=...&endMs=...&timeframeMs=...&limit=500` | Locally held candle bars and `nextStartMs` for pagination. `limit` maximum 2,000. |
| GET | `/api/v1/book?levels=20` | Best prices, spread, up to 200 levels per side, band and receive time. |
| GET | `/api/v1/trades?windowMs=60000&limit=100` | Receive-time tape, newest first, and summary of all retained matches. Window maximum 900,000 ms; limit maximum 1,000. |
| GET | `/api/v1/heatmap/walls?startMs=...&endMs=...&priceMin=...&priceMax=...&minQty=0&limit=20` | Ranked recording heatmap cells from the loaded window. `limit` maximum 100. |
| GET | `/api/v1/screenshot?name=review&target=main` | Existing screenshot result; `target` is `main`, `heatmap`, or `lab`. |
| POST | `/api/v1/symbol` | JSON `{"symbol":"ETH-USD"}`; subscribes through the chart's symbol path. |
| POST | `/api/v1/timeframe` | JSON with `heatmapTimeframeMs` and/or `candleTimeframeMs`; v1 links them and requires an advertised served timeframe. |
| POST | `/api/v1/viewport` | JSON with paired `startMs,endMs`, paired `priceMin,priceMax`, and/or `followLive`. |
| POST | `/api/v1/layers` | Partial boolean map for `heatmap`, `candles`, `footprint`, `tpo`, `volumeProfile`. |
| GET | `/api/v1/operations/<id>?waitMs=5000` | Current operation state; waits at most five seconds for a rendered frame. |
| GET | `/screenshot?name=review&target=main` | Legacy screenshot route and response, retained for existing agents. |

State, viewport, candles, book, trades and walls accept optional `symbol=<active-symbol>`; a different symbol returns `409`. `servedTimeframesMs` comes only from the server's advertisement. An older server that omits it yields `null`, distinct from an advertised empty array. Other unavailable configuration fields and unseen receive timestamps also yield `null`. `selectionEpoch` is a decimal string and advances when the active symbol or timeframe changes, or connection status changes. `sessionId` changes on each GUI run. `viewportVersion` is a decimal string when the viewport is valid. An unknown viewport field is `null`.

Successful state and viewport responses have `{"ok":true,"meta":{"sessionId":"...","symbol":"BTC-USD","selectionEpoch":"1","observedAtMs":1790596800000,"source":"gui-cache","stale":false,"coverage":"unknown","truncated":false},"data":{...}}`. Errors have `{"ok":false,"error":{"code":"not_found","message":"Unknown route"}}`. Screenshot success retains `{"ok":true,"path":"./screenshots/review.png","target":"main"}`. The v1 route also accepts `afterOperation=<id>&waitMs=0..5000`; it captures only after that operation renders, returning `408 render_timeout` if no frame arrives, `409 operation_not_rendered` if superseded or failed, and `404 unknown_operation` for an unknown ID. A guarded screenshot includes decimal-string `frameId`, `viewportVersion` and `selectionEpoch`. Screenshots are limited to one request per second (`429`).

Requests must use HTTP/1.1 and a `Host` of `localhost` or `127.0.0.1`, with an optional port. A browser `Origin` is rejected. Headers are capped at 8 KiB and bodies at 16 KiB; POST controls require `Content-Type: application/json`. GET requests do not accept bodies. The server allows eight concurrent connections and closes stalled requests. Sockets and handlers currently run on the GUI thread; operation waits use short timer callbacks and do not block the event loop. PNG encoding/writing remains synchronous and can pause the GUI.

## Controls and render ordering

Every successful POST returns an `operationId` and `status:"applied"`, plus the changed state. GET `/operations/<id>` returns `applied`, `rendered`, `superseded`, or `failed`; it includes `viewportVersion`, and a rendered operation includes `frameId`. A later control of the same kind supersedes a pending one. `waitMs` expires with the current state (usually `applied`) and does not imply failure. Unknown IDs return 404. Operations are retained for the newest 256 IDs.

`/symbol` accepts uppercase `BASE-QUOTE` symbols with two to twenty ASCII alphanumeric characters on each side. `/timeframe` requires an advertised `servedTimeframesMs` entry; older servers without that advertisement cannot select a timeframe through this API. Either field sets both v1 timeframes, and unequal values return 422. `/viewport` requires each bound pair to be complete, finite, positive for price, and increasing. Explicit time bounds turn off follow mode; neither bound pair can be combined with `followLive:true`. Omitted price bounds preserve the current price range. Follow-only requests preserve the current price span and recenter it on the latest best bid/ask midpoint, falling back to the last trade until a two-sided book arrives. An explicit price range disables follow mode. Viewport changes use `setViewport()` so the version advances.

The render acknowledgement is an ordering guarantee: the renderer copies the GUI control revision, selection epoch and viewport version into its frame context, and publishes fixed-size atomic frame data after rendering. It does not assert that history is complete or that live data did not advance. A hidden chart can leave an operation `applied`, so a guarded screenshot can time out. Existing layer setters may disable conflicting layers; the response reports the resulting full layer state.

## Local evidence reads

All three reads use the same `ok/meta/data` envelope as state and viewport, with UTC epoch milliseconds. `startMs,endMs` is half-open: a candle whose start equals `endMs` is excluded. Candles are copied from the GUI's local series only; a timeframe without a local series returns `422 timeframe_unavailable` and no history request is sent. `nextStartMs` is the actual start of the next retained bar, or `null`. Gaps in locally held history mean candle coverage remains `partial` when bars exist.

Book levels are price/quantity pairs, bids descending and asks ascending. Missing best prices and spread are `null`. `bandLimited` is true because the replica only covers its configured band; `band` is null until it is initialized. A preallocated occupancy bitmap finds populated levels; the read examines at most 16,384 bitmap words (covering 1,048,576 price slots) per side under the book lock. If it cannot reach all requested levels, `scanLimited` and `meta.truncated` are true; a null best price in that case does not prove the full band side is empty. `receivedAtMs` is the GUI's most recent book receive time.

Trades are held in a preallocated GUI ring, capped at 10,000 rows and 15 minutes. Each row has `receivedAtMs`, `eventTimeMs:null`, `id`, `side`, `price` and `qty`; `timeBasis` is `received` because wire event time is currently discarded. The summary counts and sums every retained trade in the requested window, even when `limit` returns fewer rows. `meta.truncated` marks that row limit; `retentionLimited` marks a capacity eviction inside the requested window. A symbol, timeframe or reconnect epoch change excludes prior epoch rows.

## Heatmap walls

`GET /api/v1/heatmap/walls` reads only the currently loaded recording projection; it never requests history. Omitted time bounds use the loaded window `[start,end)`. Provided bounds select bucket starts within that window, and price bounds include cells whose price band overlaps the requested band. `minQty` defaults to 0 and must be finite and nonnegative. `limit` defaults to 20 and is capped at 100. The scan rejects more than 16,000,000 candidate cells with `422 scan_limit`; narrow the time range. An empty or not-yet-placed window returns an empty result and `coverage:"unknown"`.

The standard envelope contains `basis:"recording-twap-sum"`, `bandTick`, `loadedRange:[startMs,endMs]`, `recordedColumns`, `missingColumns`, a note, and `walls`. Each wall has `bucketStartMs`, `priceLow`, `priceHigh`, `side:"bid"|"ask"`, `qty`, `notional`, and `forming:false`. `qty` is the valid row's little-endian linear quantity code multiplied by that column's scale. `notional` uses the midpoint of the price band. These are aggregated resting sizes per cell, not individual orders or proof of continuous persistence. Recording cells represent the dominant side at each row. Ranking is descending quantity, then ascending bucket start, price low and side (bid before ask). Zero-size and invalid rows are excluded. `recordedColumns` counts cached columns in the selected time range; `missingColumns` counts absent buckets, whether known missing or not yet fetched. `meta.coverage` is `complete` only when all selected columns are present and selected rows are valid, `partial` when some are present, otherwise `unknown`.

Legacy mode returns `409 recording_required`. Its source liquidity channel is a linear, absolute quantity (`u16 * liquidityScale`), but its side bit comes from separately normalized and thresholded intensity. The channel can select an ask while the intensity reports a bid, and legacy has no row-validity mask. It cannot produce reliable side-labeled walls or distinguish unknown rows from zero. The GPU and label rings are also display-resampled, so they are not suitable source columns for this endpoint. The API queues the bounded scan to `DataProcessor`'s worker thread and returns its small immutable result asynchronously; a selection change during the scan returns `409 selection_changed`.

## Live hand-off checks

Run `sentinel-gui` with the API enabled, then use:

With `heatmap.source: recording` and an advertising recording server, wait until recorded columns appear, then check:

```sh
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?limit=3&minQty=1'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?startMs=1790593200000&endMs=1790596800000&priceMin=64000&priceMax=65000'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?limit=101'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?startMs=2&endMs=1'
```

Use actual loaded time and price bounds from `/api/v1/viewport` for the third request. Check quantity order, validity gaps, `loadedRange`, coverage counts and the source band tick against the visible heatmap. Switch to `heatmap.source: legacy` and confirm the same route returns `409 recording_required`. A large time range can return `422 scan_limit`; narrowing it should succeed.

```sh
# The POST returns an operationId such as o1. Substitute the returned ID below.
curl -si -H 'Content-Type: application/json' -d '{"startMs":1790593200000,"endMs":1790596800000}' http://127.0.0.1:17100/api/v1/viewport
curl -si 'http://127.0.0.1:17100/api/v1/operations/o1?waitMs=5000'
curl -si 'http://127.0.0.1:17100/api/v1/screenshot?name=viewport-after&target=heatmap&afterOperation=o1&waitMs=5000'
curl -si -H 'Content-Type: application/json' -d '{"followLive":true}' http://127.0.0.1:17100/api/v1/viewport
curl -si -H 'Content-Type: application/json' -d '{"symbol":"ETH-USD"}' http://127.0.0.1:17100/api/v1/symbol
curl -si -H 'Content-Type: application/json' -d '{"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}' http://127.0.0.1:17100/api/v1/timeframe
curl -si -H 'Content-Type: application/json' -d '{"heatmap":true,"candles":false}' http://127.0.0.1:17100/api/v1/layers

# Negative cases: each must fail without changing the chart.
curl -si -H 'Content-Type: application/json' -d '{"startMs":1,"endMs":2,"followLive":true}' http://127.0.0.1:17100/api/v1/viewport
curl -si -H 'Content-Type: application/json' -d '{"priceMin":100}' http://127.0.0.1:17100/api/v1/viewport
curl -si -H 'Content-Type: application/json' -d '{"heatmapTimeframeMs":12345}' http://127.0.0.1:17100/api/v1/timeframe
curl -si -H 'Content-Type: application/json' -d '{"symbol":"../BAD"}' http://127.0.0.1:17100/api/v1/symbol
curl -si 'http://127.0.0.1:17100/api/v1/operations/o999999?waitMs=5000'
curl -si 'http://127.0.0.1:17100/api/v1/screenshot?name=bad&afterOperation=o999999&waitMs=5000'
```

For the hidden-window timeout case, POST a viewport control while the heatmap window is hidden, then request its screenshot with `afterOperation=<returned-id>&waitMs=100`; expect `408 render_timeout` and no file. Wait at least one second between screenshot requests to avoid the 429 rate limit. Restore the window and verify a new operation reaches `rendered` before screenshot capture.

```sh
curl -si 'http://127.0.0.1:17100/api/v1/state'
curl -si 'http://127.0.0.1:17100/api/v1/viewport'
curl -si 'http://127.0.0.1:17100/api/v1/candles?startMs=1790593200000&endMs=1790596800000&timeframeMs=60000&limit=2'
curl -si 'http://127.0.0.1:17100/api/v1/book?levels=20'
curl -si 'http://127.0.0.1:17100/api/v1/trades?windowMs=60000&limit=100'
curl -si 'http://127.0.0.1:17100/api/v1/candles?startMs=1790596800000&endMs=1790593200000&timeframeMs=60000'
curl -si 'http://127.0.0.1:17100/api/v1/book?levels=201'
curl -si 'http://127.0.0.1:17100/api/v1/screenshot?name=agent-api-v1&target=main'
curl -si 'http://127.0.0.1:17100/screenshot?name=agent-api-legacy&target=main'
curl -si -H 'Origin: http://example.com' 'http://127.0.0.1:17100/api/v1/state'
curl -si -H 'Host: example.com' 'http://127.0.0.1:17100/api/v1/state'
curl -si -X POST 'http://127.0.0.1:17100/api/v1/state'
curl -si 'http://127.0.0.1:17100/api/v1/missing'
```

Check the API metadata across a symbol switch, timeframe switch and disconnect/reconnect. Compare viewport values with the visible chart and inspect the newest GUI run log for warnings and errors. The GUI cannot be launched inside the sandbox, so these checks are for the orchestrator.

Book prices are dense-book bucket starts on `orderbook.tickSize` (see `/state`), not individual exchange price levels; sizes are the bucket totals rounded to 1e-8. Best bid and best ask are bucket prices too, so they can share a bucket and `spread` can read 0 when the real spread is under one tick.

Control (`POST`) and `GET /api/v1/operations/<id>` responses use the standard envelope: read `operationId`, `status`, `viewportVersion` and `frameId` under `data`. The screenshot response keeps its legacy flat shape.
