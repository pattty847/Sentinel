# Agent API (v1, slices 1-2)

The GUI listens on `127.0.0.1` at `gui.api_port` (default `17100`). `api_port=0` disables it. This is a local observation API; reads do not fetch history. All times are UTC epoch milliseconds. It currently exposes these routes:

| Method | Route | Result |
|---|---|---|
| GET | `/api/v1/state` | Connection, advertised server configuration, active layers and per-source last receive times. |
| GET | `/api/v1/viewport` | Active chart bounds, linked heatmap/candle timeframe, follow mode, dimensions, zoom and viewport version. |
| GET | `/api/v1/candles?startMs=...&endMs=...&timeframeMs=...&limit=500` | Locally held candle bars and `nextStartMs` for pagination. `limit` maximum 2,000. |
| GET | `/api/v1/book?levels=20` | Best prices, spread, up to 200 levels per side, band and receive time. |
| GET | `/api/v1/trades?windowMs=60000&limit=100` | Receive-time tape, newest first, and summary of all retained matches. Window maximum 900,000 ms; limit maximum 1,000. |
| GET | `/api/v1/screenshot?name=review&target=main` | Existing screenshot result; `target` is `main`, `heatmap`, or `lab`. |
| GET | `/screenshot?name=review&target=main` | Legacy screenshot route and response, retained for existing agents. |

All v1 read routes except screenshot accept optional `symbol=<active-symbol>`; a different symbol returns `409`. `servedTimeframesMs` comes only from the server's advertisement. An older server that omits it yields `null`, distinct from an advertised empty array. Other unavailable configuration fields and unseen receive timestamps also yield `null`. `selectionEpoch` is a decimal string and advances when the active symbol or timeframe changes, or connection status changes. `sessionId` changes on each GUI run. `viewportVersion` is a decimal string when the viewport is valid. An unknown viewport field is `null`.

Successful state and viewport responses have `{"ok":true,"meta":{"sessionId":"...","symbol":"BTC-USD","selectionEpoch":"1","observedAtMs":1790596800000,"source":"gui-cache","stale":false,"coverage":"unknown","truncated":false},"data":{...}}`. Errors have `{"ok":false,"error":{"code":"not_found","message":"Unknown route"}}`. Screenshot success retains `{"ok":true,"path":"./screenshots/review.png","target":"main"}`. The v1 screenshot route does not yet support operation waits or render acknowledgement.

Requests must use HTTP/1.1 and a `Host` of `localhost` or `127.0.0.1`, with an optional port. A browser `Origin` is rejected. Headers are capped at 8 KiB and bodies at 16 KiB; GET requests do not accept bodies. The server allows eight concurrent connections and closes incomplete requests after five seconds. Sockets and handlers currently run on the GUI thread, because the existing screenshot capture and state objects are GUI-owned; the bounded, short read handlers avoid cross-thread object access. PNG capture itself can still pause the GUI, so screenshot rate limiting and asynchronous encoding belong to the later screenshot slice.

## Local evidence reads

All three reads use the same `ok/meta/data` envelope as state and viewport, with UTC epoch milliseconds. `startMs,endMs` is half-open: a candle whose start equals `endMs` is excluded. Candles are copied from the GUI's local series only; a timeframe without a local series returns `422 timeframe_unavailable` and no history request is sent. `nextStartMs` is the actual start of the next retained bar, or `null`. Gaps in locally held history mean candle coverage remains `partial` when bars exist.

Book levels are price/quantity pairs, bids descending and asks ascending. Missing best prices and spread are `null`. `bandLimited` is true because the replica only covers its configured band; `band` is null until it is initialized. A preallocated occupancy bitmap finds populated levels; the read examines at most 16,384 bitmap words (covering 1,048,576 price slots) per side under the book lock. If it cannot reach all requested levels, `scanLimited` and `meta.truncated` are true; a null best price in that case does not prove the full band side is empty. `receivedAtMs` is the GUI's most recent book receive time.

Trades are held in a preallocated GUI ring, capped at 10,000 rows and 15 minutes. Each row has `receivedAtMs`, `eventTimeMs:null`, `id`, `side`, `price` and `qty`; `timeBasis` is `received` because wire event time is currently discarded. The summary counts and sums every retained trade in the requested window, even when `limit` returns fewer rows. `meta.truncated` marks that row limit; `retentionLimited` marks a capacity eviction inside the requested window. A symbol, timeframe or reconnect epoch change excludes prior epoch rows.

## Live hand-off checks

Run `sentinel-gui` with the API enabled, then use:

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
