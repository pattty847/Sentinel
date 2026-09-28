# Agent API (v1, slice 1)

The GUI listens on `127.0.0.1` at `gui.api_port` (default `17100`). `api_port=0` disables it. This is a local observation API; reads do not fetch history. All times are UTC epoch milliseconds. It currently exposes these routes:

| Method | Route | Result |
|---|---|---|
| GET | `/api/v1/state` | Connection, advertised server configuration, active layers and per-source last receive times. |
| GET | `/api/v1/viewport` | Active chart bounds, linked heatmap/candle timeframe, follow mode, dimensions, zoom and viewport version. |
| GET | `/api/v1/screenshot?name=review&target=main` | Existing screenshot result; `target` is `main`, `heatmap`, or `lab`. |
| GET | `/screenshot?name=review&target=main` | Legacy screenshot route and response, retained for existing agents. |

State and viewport accept optional `symbol=<active-symbol>`; a different symbol returns `409`. `servedTimeframesMs` comes only from the server's advertisement. An older server that omits it yields `null`, distinct from an advertised empty array. Other unavailable configuration fields and unseen receive timestamps also yield `null`. `selectionEpoch` is a decimal string and advances when the active symbol or timeframe changes, or connection status changes. `sessionId` changes on each GUI run. `viewportVersion` is a decimal string when the viewport is valid. An unknown viewport field is `null`.

Successful state and viewport responses have `{"ok":true,"meta":{"sessionId":"...","symbol":"BTC-USD","selectionEpoch":"1","observedAtMs":1790596800000,"source":"gui-cache","stale":false,"coverage":"unknown","truncated":false},"data":{...}}`. Errors have `{"ok":false,"error":{"code":"not_found","message":"Unknown route"}}`. Screenshot success retains `{"ok":true,"path":"./screenshots/review.png","target":"main"}`. The v1 screenshot route does not yet support operation waits or render acknowledgement.

Requests must use HTTP/1.1 and a `Host` of `localhost` or `127.0.0.1`, with an optional port. A browser `Origin` is rejected. Headers are capped at 8 KiB and bodies at 16 KiB; GET requests do not accept bodies. The server allows eight concurrent connections and closes incomplete requests after five seconds. Sockets and handlers currently run on the GUI thread, because the existing screenshot capture and state objects are GUI-owned; the bounded, short read handlers avoid cross-thread object access. PNG capture itself can still pause the GUI, so screenshot rate limiting and asynchronous encoding belong to the later screenshot slice.

## Live hand-off checks

Run `sentinel-gui` with the API enabled, then use:

```sh
curl -si 'http://127.0.0.1:17100/api/v1/state'
curl -si 'http://127.0.0.1:17100/api/v1/viewport'
curl -si 'http://127.0.0.1:17100/api/v1/screenshot?name=agent-api-v1&target=main'
curl -si 'http://127.0.0.1:17100/screenshot?name=agent-api-legacy&target=main'
curl -si -H 'Origin: http://example.com' 'http://127.0.0.1:17100/api/v1/state'
curl -si -H 'Host: example.com' 'http://127.0.0.1:17100/api/v1/state'
curl -si -X POST 'http://127.0.0.1:17100/api/v1/state'
curl -si 'http://127.0.0.1:17100/api/v1/missing'
```

Check the API metadata across a symbol switch, timeframe switch and disconnect/reconnect. Compare viewport values with the visible chart and inspect the newest GUI run log for warnings and errors. The GUI cannot be launched inside the sandbox, so these checks are for the orchestrator.
