# Agent API (v1)

The GUI listens on `127.0.0.1` at `gui.api_port` (default `17100`). `api_port=0` disables it. This is a local observation API; reads do not fetch history. All times are UTC epoch milliseconds. It currently exposes these routes:

| Method | Route | Result |
|---|---|---|
| GET | `/api/v1/state` | Connection, advertised server configuration, active layers, receive times (`lastReceivedAtMs`: `heatmap`, `candles`, `book`, `trades`, and since S8a the trade overlays `tpo`, `volumeProfile`; there is no `footprint` entry because the footprint is drawn from flow, not a pushed slice) and chart render statistics. |
| GET | `/api/v1/viewport` | Active chart bounds, linked heatmap/candle timeframe, follow mode, auto price scale (`autoScale`), lock to mid (`lockMid`), dimensions, zoom, viewport version and the last frame's drawn window (`drawn`). |
| GET | `/api/v1/docks` | Every available dock ID and its visible boolean. |
| GET | `/api/v1/candles?startMs=...&endMs=...&timeframeMs=...&limit=500` | Locally held candle bars and `nextStartMs` for pagination. `limit` maximum 2,000. |
| GET | `/api/v1/book?levels=20` | Best prices, spread, up to 200 levels per side, band and receive time. |
| GET | `/api/v1/trades?windowMs=60000&limit=100` | Receive-time tape, newest first, and summary of all retained matches. Window maximum 900,000 ms; limit maximum 1,000. |
| GET | `/api/v1/heatmap/state` | Tick and settings, shared service status, and the live layer: settled, drawn tf/tick, presets, live age, GPU bytes, controller stats. |
| POST | `/api/v1/heatmap/settings` | Partial chart settings; optional `persist:false`; returns an operation ID. |
| POST | `/api/v1/input` | Synthetic wheel, drag or click to the heatmap QQuickView; returns an operation ID. |
| GET | `/api/v1/heatmap/walls?from_ms=...&to_ms=...&tick=...&priceMin=...&priceMax=...&minQty=0&limit=20` | Recording walls ranked by peak resting size, defaulting to the viewport. `limit` maximum 100. |
| GET | `/api/v1/screenshot?name=review&target=main` | Existing screenshot result; `target` is `main` (legacy screen-region capture), `window` (Sentinel main-widget capture), `heatmap`, retained dock IDs (`orderBook`, `watchlist`, `screener`, `stockChart`, `paperTrading`, `sec`, `copenet`, `telemetry`), `statusBar`, `toolbar`, `chartmenu`, or `settings[:<Chart|Tick|Look|Budgets|Live|Debug|TPO>]`. `lab` and `aiCommentary` are retired. In agent-host mode `main` and unknown targets are refused; `window` is allowed. |
| POST | `/api/v1/symbol` | JSON `{"symbol":"ETH-USD"}`; subscribes through the chart's symbol path. |
| POST | `/api/v1/timeframe` | JSON with `heatmapTimeframeMs` and/or `candleTimeframeMs`; v1 links them and requires an advertised served timeframe. |
| POST | `/api/v1/viewport` | JSON with paired `startMs,endMs`, paired `priceMin,priceMax`, `followLive`, `autoScale` (the auto price scale, the "A" toggle) and/or `lockMid` (lock to mid); or `{"fit":"time"|"price"|"both"|"default"}` (the axis double-click actions). |
| POST | `/api/v1/layers` | Partial boolean map for `heatmap`, `candles`, `footprint`, `tpo`, `volumeProfile`, `ladder` (the chart-edge book ladder strip, independent of the primary layers, off by default), `wallRadar` (wall marks on the ladder; works only while `ladder` is on, off by default; this layer patch does not turn the ladder on); optional TPO look strings `tpoLayout` (`split`, `collapsed`), `tpoTheme` (`rainbow`, `calm`, `sage`, `indigo`) and `tpoPreset` (`letters`, `blocks`, `extended`, `indigo`; a preset keeps the current theme unless it names its own, and the theme and style are saved). The reply's `layers` reports all three (`tpoPreset` is `custom` after a Settings edit). |
| POST | `/api/v1/docks` | `{"visible":{"orderBook":false,"watchlist":false}}` or `{"focus":"heatmap"}`; optional `persist` boolean. |
| GET | `/api/v1/operations/<id>?waitMs=5000` | Current operation state; waits at most five seconds for a rendered frame. |
| GET | `/screenshot?name=review&target=main` | Legacy screenshot route and response, retained for existing agents. |

State, viewport, candles, book, trades and walls accept optional `symbol=<active-symbol>`; a different symbol returns `409`. `servedTimeframesMs` comes only from the server's advertisement. An older server that omits it yields `null`, distinct from an advertised empty array. Other unavailable configuration fields and unseen receive timestamps also yield `null`. `selectionEpoch` is a decimal string and advances when the active symbol or timeframe changes, or connection status changes. `sessionId` changes on each GUI run. `viewportVersion` is a decimal string when the viewport is valid. An unknown viewport field is `null`.

Viewport `startMs`/`endMs`/`priceMin`/`priceMax` and `zoom` are the stored (continuous) view that `POST /viewport` sets. `drawn` is what the last rendered frame drew (whole-pixel mapping, the raster camera): `{startMs, endMs, priceMin, priceMax, rowPx, colPx, dpr}`, where `rowPx`/`colPx` are the integer device pixels per heatmap row and per column and the bounds are the exact drawn window (ms and prices as numbers; `startMs`/`endMs` may be fractional). After a wheel or axis zoom the drawn window differs from the stored one by up to the ratio of the continuous to the drawn pixels per row/column; after a price fit they are equal. During a zoom glide or gesture (smooth zoom) `drawn` is that transitional frame: fractional bounds, with `rowPx`/`colPx` the whole pixels of the stored view's rest camera (the glide's target). `drawn` is `null` before the first frame.

State `data.serverConfig.heatmap.retentionColumns` reports the advertised `trade_overlays.retention_columns` trade-retention setting, or `null` when unavailable. It is not a render-grid dimension; `gridWidth` and `gridHeight` are not emitted.

State `data.render` contains `frameP50Ms`, `frameP95Ms`, `rateHz`, and `idle`. Durations cover CPU work on the chart render thread from scene synchronization through render command submission; they do not measure GPU completion. Percentiles use rendered frames from the preceding one second and are `null` if that window has no frames. `rateHz` counts renders in that same one-second window, including data-driven renders, so it is not an FPS capacity estimate. `idle` means no mouse, touch, wheel, or keyboard input reached the chart in the past second. The GUI refreshes these values four times a second.

Successful state and viewport responses have `{"ok":true,"meta":{"sessionId":"...","symbol":"BTC-USD","selectionEpoch":"1","observedAtMs":1790596800000,"source":"gui-cache","stale":false,"coverage":"unknown","truncated":false},"data":{...}}`. Errors have `{"ok":false,"error":{"code":"not_found","message":"Unknown route"}}`. Screenshot success retains `{"ok":true,"path":"./screenshots/review.png","target":"main"}`. The v1 route also accepts `afterOperation=<id>&waitMs=0..5000`; it captures only after that operation renders, returning `408 render_timeout` if no frame arrives, `409 operation_not_rendered` if superseded, `409 connection_cap` on a server subscription refusal, `409 switch_timeout` after an unanswered subscribe, `409 disconnected` if the connection drops during a switch, and `404 unknown_operation` for an unknown ID. A guarded screenshot includes decimal-string `frameId`, `viewportVersion` and `selectionEpoch`. Screenshots are limited to one request per second (`429`).

Requests must use HTTP/1.1 and a `Host` of `localhost` or `127.0.0.1`, with an optional port. A browser `Origin` is rejected. Headers are capped at 8 KiB and bodies at 16 KiB; POST controls require `Content-Type: application/json`. GET requests do not accept bodies. The server allows eight concurrent connections and closes stalled requests. Sockets and handlers currently run on the GUI thread; operation waits use short timer callbacks and do not block the event loop. PNG encoding/writing remains synchronous and can pause the GUI.

## Controls and render ordering

### Dock visibility

`GET /api/v1/docks` returns `data.visible`, a map of stable IDs to booleans. The IDs are `heatmap`, `orderBook`, `watchlist`, `sec`, `copenet`, `screener`, `stockChart`, `paperTrading`, and `telemetry` (only docks created in this window are listed). `visible` follows the dock's toggle action: a dock behind another tab remains true even when that tab is inactive. `POST` accepts either a nonempty partial `visible` map or one `focus` ID; focus hides every other dock, re-docks a floating target, and raises the target tab. Unknown IDs return `422 unknown_dock` with valid IDs, and hiding all docks returns `422 hide_all_docks` without changing the window. The response contains the resulting `visible` map, `persist`, and the usual operation ID; `/operations/<id>?waitMs=5000` provides the same frame acknowledgement as other controls. A focused heatmap claims the space freed by the other docks.

Saved named and last-session layouts that contain retired LabDock or AICommentaryFeedDock restore through temporary placeholders; the placeholders are removed and the surviving dock state is saved. No empty retired tab remains.

The GUI host defaults to the main checkout build. `scripts/dev/gui-shot.sh launch --build <worktree path>` uses the fixed `build/mac-clang/apps/sentinel-gui/sentinel-gui` binary in a direct child of `/Volumes/T7/sentinel-worktrees` or the fallback `<main>/.claude/worktrees`; the host resolves both the directory and binary and refuses escapes, missing/non-executable binaries, or builds without `--agent-host`. Build the branch through the FIFO build queue first; the host never builds or runs CMake. On 2026-10-03 the owner accepted unreviewed agent worktree GUI builds running outside the sandbox through the host, with the same trust given to Claude subagents that run their own GUI. Hosted runs still use an explicit symbol allowlist, disable trade/algo commands, isolate general settings and screenshots, pass a minimal environment, allow only Sentinel widget targets, limit to one session and expire after 30 minutes idle. The running host must be restarted by the orchestrator after this change lands before `--build` works.

Under `--agent-host`, every successful dock change is written immediately to the dedicated `gui-host/profile/docks.ini` file and restored after the next hosted window installs its layout. All other QSettings start fresh in each `<session>/settings` directory; screenshots stay in `<session>/screenshots`. `scripts/dev/gui-shot.sh profile-reset` deletes only the dock file while the GUI is stopped, or `launch --fresh-profile` deletes it before starting. In the owner's normal GUI, dock API changes affect the current session; QSettings changes only when the request includes `"persist":true`. A transient dock API change is excluded from the normal `_last_session` layout saved at close unless the owner manually toggles that dock afterward.

```sh
scripts/dev/gui-shot.sh docks list
scripts/dev/gui-shot.sh docks focus heatmap
scripts/dev/gui-shot.sh docks show orderBook
```

Every successful POST returns an `operationId` and status, plus the requested state. Most controls return `status:"applied"` immediately. A symbol switch returns `status:"pending"` while the old chart remains active; the operation becomes `applied` only after the server acknowledges the new subscription and the chart activates it. GET `/operations/<id>` returns `pending`, `applied`, `rendered`, or `superseded`; it includes `viewportVersion`, and a rendered operation includes `frameId`. A later symbol request for a different symbol supersedes the pending one; re-entering the same symbol keeps its operation and deadline. `waitMs` expires with the current state and does not imply failure. A server refusal returns `409 connection_cap`, an unanswered subscribe returns `409 switch_timeout` after about five seconds, and a disconnect during a switch returns `409 disconnected`; each includes its message and restores the last working chart and input. An offline selection remains pending until reconnect sends the subscribe, which starts the switch deadline. Unknown IDs return 404. Operations are retained for the newest 256 IDs.

`/symbol` accepts uppercase `BASE-QUOTE` symbols with two to twenty ASCII alphanumeric characters on each side. `/timeframe` requires an advertised `servedTimeframesMs` entry; older servers without that advertisement cannot select a timeframe through this API. Either field sets both v1 timeframes, and unequal values return 422. `/viewport` requires each bound pair to be complete, finite, positive for price, and increasing. Explicit time bounds turn off follow mode; neither bound pair can be combined with `followLive:true`. Omitted price bounds preserve the current price range (gpu with the auto price scale on: fitted to the visible candles, below). Follow-only requests preserve the current price span and recenter it on the latest best bid/ask midpoint, falling back to the last trade until a two-sided book arrives. An explicit price range disables follow mode. Viewport changes use `setViewport()` so the version advances.

Auto price scale. `autoScale` (GET and POST) is the TradingView-style "A" toggle in the chart's axis corner, on by default. While on, the price range is the visible candles' high/low plus a 6% margin each side, refitted in the same viewport change as every time change (pan, wheel, time-axis zoom, follow-live step, timeframe switch, time bounds) and on candle updates or history pages in view (one change, none when the fit is unchanged); with no visible candle the price stays. While on, chart drags and keyboard pans move time only and the chart wheel zooms time only; during a drag the fit follows the window on screen (the drag commits only at release), so revealed candles fit as they appear. A price-axis drag or wheel, Shift+wheel, or explicit price bounds (even equal to the current ones) turn it off; off, the price range stays where it was left, also across timeframe switches, and drags pan price again. A request resolves its flags (`autoScale`, `followLive`, bounds) and final window first and commits them as one viewport change (e.g. `{"autoScale":true,"followLive":true}` is one change at the live edge, fitted). `autoScale:true` fits at once (as the price-axis double-click; with nothing to fit yet it is on and the next candles fit; with time bounds the bounds land fitted); it cannot come with price bounds (`422 invalid_auto_scale`). (`409 auto_scale_unavailable` was the legacy renderer's answer; it is no longer returned since S8a.) A symbol switch keeps the state: on, the new symbol's candles fit; off, the span is carried as a fraction of the price with the current price (book mid, last trade, decoded data, newest close) at the same screen height, applied at the new symbol's first usable price (a symbol left before it got one passes the earlier carry on; a user price action cancels it). Until the new symbol's price scale is known (its own recorded availability; also before the first snapshot exists) the price-span limits are unknown (no clamp from another symbol's or the fallback scale); once known the carry is applied again from the then current price. A live-only symbol (no recorded availability) never gets one: 2 s after its first usable price the carry is applied from the live price and consumed, and that symbol has no Manual max span (nor zoom-in floor) until a recorded scale arrives. Under a Manual tick whose 1 row/px limit cannot hold the candles, the tick is kept and the max span is centred on the current price (book mid, then last trade, then the newest close).

Time (gpu): a timeframe switch keeps the bar width (the span scales by the timeframe ratio, inside the 1 column/px limit) and, when the Now column (the live bucket) is in view or the view follows live, keeps the Now column's screen x; a historical view keeps its end. A symbol switch keeps the time window.

Lock to mid. `lockMid` (GET and POST; the reply carries it too) is the chart corner's "M" toggle, off by default and not persisted. While on, the price window keeps its span and stays centred on the book mid (the last trade while the book is one-sided or withdrawn, back to the mid on the next two-sided top), gliding there each frame by a critically damped spring on the stored window (whole device pixels, at rest within half a device pixel of the mid); time follows live as before. It replaces the auto price scale: `lockMid:true` turns `autoScale` off, and `autoScale` turning on (the request flag, `fit:"price"`/`"both"`/`"default"`, the price-axis double-click, P) turns the lock off. Chart drags pan time only; the chart wheel and pinch zoom price about the locked centre and the lock keeps the new span. Price-axis wheel, drag and clicks, keyboard price pans and explicit price bounds turn it off (the reply then reports `lockMid:false`). `lockMid:true` cannot come with price bounds or `autoScale:true` (`422 invalid_lock_mid`), nor with `fit` (`422 invalid_fit`).

`fit` stands alone (with bounds, `followLive` or `autoScale` it returns `422 invalid_fit`): `time` shows the data's available time range (recording availability, else the candle series) clamped to the 1 column/px max span, ending one follow-live padding past the live bucket when following live or when all of it fits, else keeping the view centre inside the data (with the auto price scale on, the price follows); `price` (the price-axis double-click) turns the auto price scale on and fits the visible candles, or with no visible candle keeps the span and centres the live price (book mid, last trade, decoded data); `both` does both, all or nothing (if either part cannot fit, nothing changes); `default` (the time-axis double-click) resets to the default view: auto price scale and follow mode on, the initial span (initial_column_px per bar) ending one follow-live padding past the live bucket, price fitted. Each is one viewport change; `time`, `price` and `both` keep follow mode. With nothing known yet (no live anchor for `default`, no price) it returns `409 fit_unavailable` and changes nothing (the auto price scale and follow mode stay as they were).

The render acknowledgement is an ordering guarantee: the renderer copies the GUI control revision, selection epoch and viewport version into its frame context, and publishes fixed-size atomic frame data after rendering. It does not assert that history is complete or that live data did not advance. A hidden chart can leave an operation `applied`, so a guarded screenshot can time out. The layer map changes only the layers it names and can leave several primary layers on at once (a toolbar click returns to the exclusive state); the response reports the resulting full layer state.

## Heatmap plumbing (S6a) and the GPU renderer (S6b)

S6b: `heatmap/state` supplies the chart layer's values: `settled`, `tickMode`, `tickUnits` (the drawn tick, integer price units), `tick`, `manualTickUnits`, `offeredPresets` (units), `drawnTimeframeMs`, `drawnTickUnits`, `indicatorText` (the veil indicator, empty when none), `residentBytes`, `gpuBytes`, `liveVersion`, `liveAgeMs` (data age of the newest drawn live version at its first draw), `liveAgeP50Ms`/`liveAgeP95Ms`/`livePublishP95Ms` and `liveSamples` (over the drawn live versions since start), `prepareMs`, `lastBinMs`, `gpuFrameMs`, slot counts and `controllerStats` (refreshed asynchronously: `null` on the first read). `POST /api/v1/heatmap/settings` now drives the chart: tick mode/manual tick, palette, sensitivity, opacity, crossfade and budgets apply at once. `state.lastReceivedAtMs.heatmap` is the receive time of the newest live version. `GET /api/v1/heatmap/walls` supports gpu mode through the S7a CPU cell query described below. Wheel, axis drags, `POST /api/v1/viewport` and `/input` obey the same zoom clamps in gpu mode (GridViewState max spans; wheel and axis zoom-in also stop at 4 columns and 4 rows of the Manual tick or the finest offered preset). Wheel clicks move along the zoom ladder (smooth zoom) and axis drags scale the spans continuously (no relative zoom factor), so the wheel zooms out to the max span from any view, including one an axis drag widened. `POST /api/v1/timeframe` in gpu mode keeps the columns on screen (the span scales with the timeframe about the Now column when it is in view or following live, else about the view end, clamped), so the viewport version advances with the timeframe (S6d). Screenshots are limited to one per second per process, so pace guarded shots accordingly. A drag left without `dragMove`/`dragEnd` for 10 s is released at its last position.

S6c: `heatmap/state` adds `ui`: `toolbar` (`enabled` false while no chart is attached, `mode`, `shownTickUnits` = the drawn tick in Auto or the locked preset in Manual, `presets` = the offered presets, `presetText`, `veiled` = the resolution indicator is non-empty), `telemetryVisible` (the dock is on screen: shown and, when tabified, the current tab), `telemetryPreferred` (the saved `showTelemetry`), `settingsDialogOpen`, `budgets` (the process tiers: `decodedChunkBytes`, `spanSourceBytes`, `cpuCeilingBytes`). A `heatmap/settings` POST goes through the chart's settings model, the same object the settings dialog, the toolbar tick selector and the telemetry dock use, so they all show the change at once (`showTelemetry` shows or hides the dock). The widget screenshot targets `window`, `settings` (opens the Heatmap Settings dialog; `settings:<Tab>` selects a tab), the retained docks, `statusBar`, `toolbar` and `chartmenu` grab the widget's own painting (`QWidget::grab`); the `stockChart` QQuickView is composited from `grabWindow()` into its dock image. `window` grabs only the Sentinel main widget and composites the visible heatmap and stock-chart QQuickViews from `grabWindow()`; it never samples the screen. Hidden or inactive-tab docks return `<target>_not_visible` via `screenshot_failed`; show or focus them using `/api/v1/docks` first. `telemetry` refreshes before capture. These grabs never sample screen pixels (FM-120).

S7b: gpu-mode `heatmap/state` adds `labels`: `posted`, `results`, `retries`, `failures` (label window requests and their outcomes), `drawing` (a result matched to the drawn picture is drawn now) and `liquidityLo`/`liquidityHi` (the label window's 5th percentile and maximum cell size: the range slider's ends). `ui.toolbar` adds `shown` (which toolbar controls the active layers show: `tickSelector`, `palette`, `liquidity`, `rangeSlider`, `candleStyle`, `tpoSession`, `tpoLayout`, `labelsToggle`; the legacy `thresholdSlider` went in S8a) and `liquidityRange` (`low`/`high` handles, `endLo`/`endHi` ends). The widget target `chartmenu` grabs the chart menu (gear) and `settings:Chart` the dialog's Chart tab. Labels are drawn only from the current cell-query result whose timeframe and tick are the drawn picture's and this frame's target, only on columns whose drawn content (span build generations, live version) is the one the result was computed from; none during a crossfade or a hold.

`GET /api/v1/heatmap/state` uses the standard `ok/meta/data` envelope and accepts only optional `symbol=<active-symbol>`. `tickMode` is `auto` or `manual`; `tickUnits` is the effective manual tick (integer price units, price times the source's `priceScale`) in manual mode and `null` in auto mode. `settings` contains the complete effective chart model, including any process-only overrides. The fields `offeredPresets`, `settled`, `drawnTimeframeMs`, `drawnTickUnits`, `indicatorText`, `residentBytes`, `gpuBytes`, `liveVersion`, `liveAgeMs`, and `controllerStats` are `null` until the S6b chart layer supplies them. `service` reports `connected`, `availabilityReady` for the active symbol, `committedCpuBytes`, and `chunkRequests` (stats refreshed at 4 Hz). No controller, historical fetch or live subscription is created by this read.

`POST /api/v1/heatmap/settings` accepts any nonempty subset of `tickMode`, `manualTick`, `minRowPx`, `hysteresis`, `crossfadeMs`, `showBandEdges`, `palettePreset`, `bidGradient`, `askGradient`, `sensitivityMin`, `sensitivityMax`, `opacity`, `gpuCapBytes`, `uploadBudgetBytes`, `prefetchTiles`, `liveMinIntervalMs`, `showTelemetry`, `showLabels`, `labelCurrency` (`usd`/`asset`), `labelMinPx`, `labelMaxPx`, `candleUpColor`, `candleDownColor`, `candleWickColor`, `candleBodyOpacity`, `candleWickWidth`, `showVolumeBars`, `volumeBandPercent` (10..40), `volumeOpacity` (0.1..1), `showLadder` (the chart-edge book ladder, also the chart menu's "Order book ladder"), `ladderWidthPx` (integer strip width, 60..240, default 120), `showWallRadar` (boolean, default false; the chart menu's "Wall radar") and `wallMultiple` (the wall threshold as a multiple of that side's median nonempty display bin among the 32 nearest its touch, 1.5..20, default 4). Candle colours use `#RRGGBB`; `candleWickColor` also accepts `auto` (the default, each wick follows its body); `candleBodyOpacity` is 0..1; `candleWickWidth` is integer 1..3 device pixels. Unknown keys, wrong types, invalid enums and malformed colours or gradients return 422 without changing settings. Finite numeric settings clamp to the limits in [CONFIG.md](CONFIG.md#heatmap-chart-settings-s6); fractional values for integer fields are rejected. The response contains the normalized settings/state and `operationId`/`status`. Process CPU budgets are shared settings, outside this per-chart patch.

The optional boolean `persist` defaults to `true`. Use `"persist":false` for an agent process: no QSettings or shared manual tick memory is written. At least one chart field is required (`persist` alone is invalid). Later persisted patches save only their supplied fields against the latest stored model, so an unrelated opacity change cannot persist a temporary override. The response echoes `persist`. Only an explicit `manualTick` in a persisted manual-mode patch updates shared symbol/timeframe tick memory; entering manual mode alone leaves that memory intact. Named workspace snapshots contain persisted settings, excluding process-only overrides. `_last_session` neither saves nor restores heatmap settings; live `heatmap/<chartId>/...` values remain the startup source of truth after a crash. Explicitly restoring a named workspace replaces the effective chart settings and persists that snapshot.

`POST /api/v1/input` accepts:

```json
{"kind":"wheel","target":"chart","x":300,"y":200,"deltaY":120,"modifiers":["shift"]}
```

`kind` is `wheel`, `dragStart`, `dragMove`, `dragEnd`, `click`, or `doubleClick` (press, release, press, double-click, release, as Qt 6 delivers it; on `priceAxis` it turns the auto price scale on and fits, on `timeAxis` it resets to the default view). `target` is `chart`, `priceAxis`, or `timeAxis`. Required `x,y` are finite, nonnegative logical pixels **relative to the target region's top-left**, strictly inside that region (right/bottom edge excluded). The regions use the current renderer geometry: chart content, its right price gutter, or its bottom time gutter; the corner is excluded. Values above 1,000,000 are rejected at decode time. `deltaY` is required only for wheel, an integer in -12000..12000 excluding zero, in Qt angle-delta units (120 is one wheel notch: one zoom rung, at most 4 per event; a remainder below 120 carries to the next wheel). Omit it for mouse kinds. Optional `modifiers` is a distinct array drawn from `shift`, `control`, `alt`, `meta`. Unknown fields, missing coordinates, invalid kinds/targets/modifiers or out-of-region positions return 422 before sending an event.

Drags use the left mouse button. `dragStart` presses, `dragMove` moves with that button held, and `dragEnd` releases it. Move/end require an active drag on the same target; other inputs during a drag return `409 drag_active`. Unmatched move/end return `409 no_drag`. A click sends a press/release pair. Each wheel request sends exactly one `QWheelEvent`; the host sends events synchronously on the GUI thread through `QCoreApplication::sendEvent` to the heatmap dock's `QQuickView`. Successful requests return an operation ID with the same frame acknowledgement rules as `/viewport`; they do not promise data completeness. A wheel zoom glides to its rung over about 130 ms (smooth zoom): the acknowledged frame can be a transitional one, so wait about 150 ms (or until `viewport.drawn` stops changing) before a screenshot of the landed view. A missing view returns 503.

```sh
curl -s 'http://127.0.0.1:17100/api/v1/heatmap/state'
curl -s -H 'Content-Type: application/json' -d '{"tickMode":"manual","manualTick":100,"persist":false}' http://127.0.0.1:17100/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"kind":"wheel","target":"chart","x":300,"y":200,"deltaY":120}' http://127.0.0.1:17100/api/v1/input
# Use the returned operation ID; chart evidence uses target=heatmap (FM-120).
curl -s 'http://127.0.0.1:17100/api/v1/screenshot?name=wheel&target=heatmap&afterOperation=o3&waitMs=5000'
```

## Footprint cells

`paperPositionStyle` is a per-chart look setting on `POST /api/v1/heatmap/settings`: `"tag"` (default) shows a compact side, size and live PnL pill on the visible entry line; `"band"` restores full-width entry-to-mark PnL shading. The tag hides when entry is off-screen. Both values apply live and are returned in `heatmap/state.settings`.

`POST /api/v1/layers` accepts the following persistent chart settings. The same
keys work in `POST /api/v1/heatmap/settings` (including `persist:false`) and appear
in `GET /api/v1/state` under `layers`. QML exposes them on
`unifiedGridRenderer.layers.<key>`.

| Key | Values / default | Settings → Chart |
| --- | --- | --- |
| `footprintIntensity` | `bar` (default), `window` | Footprint → Volume scale |
| `footprintCandle` | `spine` (default), `full` | Footprint → Candle |
| `footprintNumbers` | boolean, true | Footprint → Show numbers |
| `footprintDustNotional` | finite nonnegative quote amount, 2500 | Footprint → Dust threshold (quote currency) |
| `flowFooter` | boolean, false | Order flow → Show flow footer |
| `cvdStripPx` | integer 0..400, 0 | Order flow → Strip height |
| `cvdReset` | `visible` (default), `utcDay`, `tpoSession` | Order flow → Reset |
| `cvdStyle` | `line` (default), `area` | Order flow → Style |
| `cvdFillAnchor` | `auto` (default), `zero`, `edge`, `none` | Order flow → Area fill anchor |

`footprint:true` shows the footprint cells; there is no source choice. The former
`footprintSource` key is gone: a request carrying it gets the normal unknown-field
`422` on both routes. The cells feed runs only when footprint is visible.
Cells enter at 24 logical px per column and 6 per row. Numbers enter at 96.013/17,
calculated from two maximum-width five-glyph labels at 13 px, their centre gaps,
and the candle space, plus the retention margin. Both stages retain a 2 px
zoom-out hysteresis band. Numbers exit at the fit boundary (94.013/15), so the
widest labels fit throughout the retained stage.

A card covers the heatmap across the bar's low-to-high rows, including gaps;
intersecting heatmap label boxes are omitted. Bid fills extend left from the
centre spine, ask fills right. Length uses the maximum single-side cell volume
of the bar or visible window. Numbers align toward the centre. Zero sides are
blank; positive magnitudes below .01 use `<.01`; other base-unit values use
magnitude-dependent decimals and k/M/B (at most five characters, never scientific
notation). Values round decimal half-up with carries across magnitude and suffix boundaries.
Suffixed values use three significant digits and trim trailing fractional zeros
(including carries such as `999.6 → 1k`). Magnitudes rounding to one trillion or
above use `>999B` to retain the width bound without understating the value.
CVD gutter min/max/last values share this formatter, with a minus sign for negative
values and `0` for exact zero. Nonzero magnitudes below .01 use `<.01` or `-<.01`.

Dust sides (below the quote threshold or below .01 base units) have no fill and
22% text alpha. Other text is muted, or bright above 55% of the bar's maximum
single-side cell volume. `footprintNumbers:false` keeps the Cells stage.
`footprintCandle:spine` draws a left 6 px body pill and 1.5 px wick on covered bars;
`full` retains the chosen candle style. Bars without flow retain normal candles.
The removed hollow-candle setting is rejected by the API.

```sh
curl -s -H 'Content-Type: application/json' -d '{"footprint":true,"footprintIntensity":"bar","footprintCandle":"spine","footprintNumbers":true,"footprintDustNotional":2500}' http://127.0.0.1:17115/api/v1/layers
```

## Cumulative volume delta strip

`POST /api/v1/layers` and `POST /api/v1/heatmap/settings` accept these keys:

| Key / QML property on `layers` | Values | Default |
| --- | --- | --- |
| `flowFooter` | Boolean; per-bar DELTA / CUM / VOL footer | `false` |
| `cvdStripPx` | Integer logical px, clamped to 0..400 by API and QML setters; 0 disables. Drawing also clamps to surface height. | `0` |
| `cvdReset` | `visible`, `utcDay`, `tpoSession` | `visible` |
| `cvdStyle` | `line`, `area` | `line` |
| `cvdFillAnchor` | `auto`, `zero`, `edge`, `none` | `auto` |

Both routes report the current knobs; `GET /api/v1/state` includes them under `layers`.
Layer-route and QML writes persist, including writes of the current value. The heatmap
settings route supports `persist:false` for lab comparisons without changing saved settings.
CVD works with footprint on or off and has no footprint zoom gate. With area style,
`auto` fills to zero when visible and to the near plot-band edge when visible
samples stay on one side of zero. `edge` also uses that plot-band edge for
one-sign data, and fills to zero when data crosses zero. `zero` forces zero
fill; `none` draws the line only.
Sub-second charts use the loaded 1 s flow series.
The default `visible` mode anchors at zero on the first bar whose start time is at
or after the drawn window's left edge (`drawnStartMs`), then accumulates subsequent
bar deltas up to (excluding) the right edge. The label is “CVD since view start”.
Panning recomputes the anchor: overlapping points shift vertically by a constant,
while their pairwise differences stay unchanged. Publishing visits only visible
bars after a lookup of the left edge; this mode needs no extra history.
All modes use base asset units, autoscale visible values together with zero, and
replace the forming bar's previous value.

`utcDay` and `tpoSession` sum loaded bars from their reset boundary, including bars
outside the viewport. `tpoSession` uses the configured `tpo.tpoSessionType` and
resets at its session open; accumulation continues until the next open, including
outside session hours. Only these two modes, while the strip or footer is enabled, extend
flow requests and retention to the earliest visible flow bar's reset boundary,
within the existing 8,192-bar limit. Panning or zooming inside a fully loaded reset
period preserves its baseline. Until history reaches the boundary, or when the
cap excludes it, the label says “CVD since HH:MM loaded” (with a UTC date across
days). Complete coverage of earlier empty buckets counts as loaded. Each visible
reset period tracks its own loaded start; any truncated period makes the label explicit.
With both strip and footer disabled, or in `visible` mode, CVD keeps the usual viewport history window.

The removed `none` value is rejected by both API routes; QML writes leave the
current value unchanged. A persisted `none` is normalized to `visible` on load,
including saved layouts.

The opaque `bg0` strip overlays the bottom of the chart surface and spans the plot,
ladder gutter and price gutter, leaving price mapping unchanged and excluding the time
axis. A 1 px `line` divider marks its top; price-axis labels stop above that edge.
`area` fills to zero in green above and red below; `line` draws a 1.5 px cumulative curve.
The price gutter shows maximum, minimum and last CVD values from visible samples,
not the endpoints of the zero-inclusive plot scale. The last-value label follows the
curve's vertical mapping, clamped to the gutter and clear of the extrema labels.
Labels use 13 px MSDF text in 17 px rows with 2 px spacing: from 21 px strip height,
show last only; from 40 px, show min/max; from 59 px, show all three. The reset title
gets a separate plot band only from 78 px, after the three value rows fit. A label
that exceeds the available width is omitted whole.
The optional footer uses three 13 px rows (55 px including padding), docked above the
strip or at the surface bottom when the strip is off. DELTA uses signed flow-bar
delta with an explicit positive `+` (magnitudes below .01 show `0` without a pill),
a CVD-green/red pill scaled to visible maximum
absolute delta; CUM reads the exact CVD value and reset anchor; VOL uses unsigned
flow-bar volume. No flow bar means no values. The atlas keeps ASCII row labels
DELTA / CUM / VOL. Narrow bars show every k-th absolute bar index. The stride uses the widest possible
formatted value at the current atlas size, so panning, re-anchoring and live magnitude
changes preserve its phase. The footer hides when the remaining surface
is shorter than its three rows.
Ladder bottom edge pills and row badges stay above the combined footer and strip
when matching flow data is available for the current timeframe. Candles, trade bubbles,
and TPO/VP subtrees are clipped above the same pane; price mapping stays unchanged. With no drawable CVD data, ladder badges keep their full height.
Missing flow data draws nothing. No default or golden scene changes until enabled.

On an isolated lab at port 17116 (each command is process-only):

```sh
curl -s -H 'Content-Type: application/json' -d '{"cvdStripPx":96,"persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdReset":"visible","persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdReset":"utcDay","persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdReset":"tpoSession","persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdStyle":"area","persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdStyle":"line","persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
curl -s -H 'Content-Type: application/json' -d '{"cvdStripPx":0,"persist":false}' http://127.0.0.1:17116/api/v1/heatmap/settings
```

To save a selection, omit `persist:false`, or post the knobs to `/api/v1/layers`.

## Trade bubbles (GPU chart)

The toolbar gear menu has **Trades** (off by default) and **Minimum trade notional
(quote)** presets: All trades, Auto (visible median), 100, 1,000, 10,000 and 100,000. Notional is
price times asset size, in the pair's quote currency (USD for BTC-USD, ETH-USD, etc.).
The Chart settings tab exposes **Bubble placement** and All / Auto / Custom minimum controls.
The same per-chart/named-layout settings are exposed in `heatmap/state` and accept:

```json
{"showTrades":true,"bubblePlacement":"auto","tradeMinNotional":0,"persist":false}
```

POST this to `/api/v1/heatmap/settings`; `persist` defaults to true, and the normal
operation/render acknowledgement applies. `showTrades` is boolean;
`tradeMinNotional` accepts finite numbers clamped to -1..1e12: 0 (default)
adds no quote filter (the base-size style minimum still applies), -1 hides marks below the visible median, and a positive value
sets a fixed minimum in quote currency. `bubblePlacement` accepts exactly:

- `individual` (default): exchange time and execution price, with sweep merging.
- `slot`: one aggregate per epoch-aligned (bar, side), at the bar centre and side
  VWAP, with summed side notional. Also draw that bar's three largest merged
  executions at the bar centre, ranked across both sides from the retained live tape.
- `auto`: initially individual at a column width >= 40 logical pixels, slot below
  40. Once selected, switch to individual at >= 42 and to slot at <= 38, retaining
  the previous mode between those limits. Timeframes <= 1 second always select
  individual. Explicit `slot` also works at those short timeframes.

Placement and minimum persist per chart/named layout, support `persist:false`, and
reset with the Chart tab. Slot aggregation includes all retained trades in each
bar overlapping the time window, including its offscreen portion; the aggregate
and notable marks each pass the minimum independently. Notables also contribute
to the aggregate; their extra marks identify large executions, not additional volume.
A notable is omitted when it is the sole contributor to its side aggregate.

In every mode, consecutive same-side trades with identical exchange timestamps
(including sub-millisecond precision) and consecutive unsigned numeric IDs merge
into one sweep capsule. Its endpoints are the lowest and highest execution prices;
its notional is summed, and `sweepLevels` counts distinct prices. Gaps, nonnumeric
IDs, side changes and different timestamps break the run. Sweep merging precedes
minimum filtering. A price change from the preceding retained trade gives full
bubble opacity. The first execution without a predecessor is passive; same-price passive executions use 35% of that opacity. A sweep
with any price change uses full bubble opacity.

Default sizing uses `radius = clamp(2, 18, 9 * sqrt(notional / ref))` in logical pixels.
The buy/sell style radius rescales this curve (18 px by default, editable from 4–48 px).
Individual marks use the p95 of individual trade notionals in the visible
half-open **time** window. Side aggregates and their notable marks use the p95
of the side-bar aggregate notionals drawn in Slot mode. Each notable's radius
is capped at its side aggregate's radius. Auto minimum uses the visible lower median
of individual trades. The p95 and median are quantised to quarter-octave steps;
the active value changes only after a move of more than one step. The policy uses
linear-time selection and recomputes at most every 250 ms as window contents
change, including during panning and ordinary zooming. A placement mode,
timeframe or minimum change, or a window-width change of at least 2x, can
recompute sooner but respects a 100 ms minimum interval. First entry into Slot
computes its aggregate reference immediately so marks are visible on that frame.
When a reference update is deferred, the bubble overlay requests a frame at its
deadline; chart settlement waits until that update has completed.
Capsule half-width preserves circle area
when possible, but is floored at 2 logical pixels for long sweeps, adding area
when needed for visibility. The size clamps limit scaling at the extremes.

**Trades above candles** defaults on (`settings.tradesAboveCandles: true`). Set it
to false to draw below candles; both orders remain above the heatmap and below
algo/order overlays. Buy/sell use the app's direction tokens (`#2EBD85` / `#F6465D`),
independent of the heatmap palette. Default fill opacity is 40%, or 14% for passive marks.
The enabled inward ring defaults to `#0F1114`, 1 logical px and full opacity; passive
marks apply the same 0.35 multiplier to the ring. Settings → Chart → Trade bubbles
edits each element's colour, opacity, visibility and radius/width; saved values override
client YAML defaults. See [Trade bubble style](CONFIG.md#trade-bubble-style).

The additional base-size minimum defaults to 0.38 BTC for BTC-USD. It filters completed
marks by summed base size after sweep/slot placement; sub-minimum fills still contribute
to their sweep or Slot aggregate's size and VWAP. Other USD products derive an equivalent
notional minimum from observed BTC/product prices, with the existing $100,000 BTC reference
until a BTC price arrives. Automatic minimums use two significant figures and a >5% change
threshold. Editing appearance does not pin the minimum; only a minimum-control edit
creates a per-product override.
Larger marks draw first so smaller executions stay on top.

The 4,096-mark geometry cap remains. Nearby marks stay separate within this budget.
Above it, each side aggregates into
6 px spatial bins with notional-weighted centres, then the smallest fitting 6 px
multiple if needed. This density fallback applies to both placements, preserves
qualifying notional, and may combine slot/notable marks or separate executions;
it retains the largest capsule span in a bin at the bin centre and the strongest
opacity. All tape, policy scratch, projection scratch and QSG geometry are bounded
and reused. No historical bubble request is introduced.

Coverage is **live chart-session history only**, retained even with Trades off:
the latest 100,000 valid trades for the active symbol, surviving timeframe
changes and same-symbol reconnects. Switching symbols/clearing the chart
resets this tape. Subscribe/reconnect snapshots can replay recent trades: nonempty
`trade_id` values are deduplicated against the retained tape. IDs expire with their
rows; rows from older servers without IDs cannot be reliably deduplicated and may
repeat. No request for durable or pre-subscription raw history exists;
empty bubbles do not establish zero traded volume. The `/trades` evidence endpoint
uses its separate 10,000-row/15-minute tape, so it is not the full bubble cache.

## Local evidence reads

All three reads use the same `ok/meta/data` envelope as state and viewport, with UTC epoch milliseconds. `startMs,endMs` is half-open: a candle whose start equals `endMs` is excluded. Candles are copied from the GUI's local series only; a timeframe without a local series returns `422 timeframe_unavailable` and no history request is sent. `nextStartMs` is the actual start of the next retained bar, or `null`. Gaps in locally held history mean candle coverage remains `partial` when bars exist.

Book levels are price/quantity pairs, bids descending and asks ascending. Missing best prices and spread are `null`. `bandLimited` is true because the replica only covers its configured band; `band` is null until it is initialized. A preallocated occupancy bitmap finds populated levels; the read examines at most 16,384 bitmap words (covering 1,048,576 price slots) per side under the book lock. If it cannot reach all requested levels, `scanLimited` and `meta.truncated` are true; a null best price in that case does not prove the full band side is empty. `receivedAtMs` is the GUI's most recent book receive time.

Trades are held in a preallocated GUI ring, capped at 10,000 rows and 15 minutes. Each row has `receivedAtMs`, `eventTimeMs` (null when the server omits or sends an invalid exchange time), `id`, `side`, `price` and `qty`; `timeBasis` remains `received` for this endpoint's window and summary; `eventTimeMs` now preserves the exchange timestamp separately. Side is normalized to aggressor (buy/sell) at live-wire decode. The summary counts and sums every retained trade in the requested window, even when `limit` returns fewer rows. `meta.truncated` marks that row limit; `retentionLimited` marks a capacity eviction inside the requested window. A symbol, timeframe or reconnect epoch change excludes prior epoch rows.

## Heatmap walls

`GET /api/v1/heatmap/walls` scans on request from client-held recording data; it never fetches history. Omitted bounds use the chart's current visible time span and its visible price span extended by one view height above and below (clamped at zero). Explicit `from_ms`/`to_ms` select a half-open period of bucket starts and must be supplied together. The existing `startMs`/`endMs` spellings remain accepted; supplying both spellings of one bound is an error. Optional `tick` overrides the drawn tick and must be positive, finite and representable in integer source price units. `priceMin`/`priceMax` include overlapping price cells. `minQty` defaults to 0 and must be finite and nonnegative; `limit` defaults to 20, maximum 100. A tick not representable in source price units returns `422 bad_tick`; invalid scan bounds return `422 invalid_range`. GPU scans reject more than 16,000,000 candidate cells with `422 scan_limit`; narrow the period or price band, or increase the tick. They run on the heatmap-data service's bounded worker pool. Missing picture/CPU capacity returns 503.

The standard envelope contains `basis:"recording-twap-sum"`, `bandTick`, `loadedRange:[startMs,endMs]`, `recordedColumns`, `missingColumns`, a note, and `walls`. Results also contain `tick`, `range:{from_ms,to_ms,priceMin,priceMax}` and `unknownRows`. `loadedRange` is the advertised recording availability; it does not imply that those chunks are cached. `range` is the requested scan range. Unheld buckets count toward `missingColumns`, and veiled cells set `unknownRows`. Coverage is `complete` only when all selected columns are present and selected rows are valid, `partial` when some are present, otherwise `unknown`.

Each wall groups one price cell and side across time. Fields are `bucketStartMs` (the earliest peak), `priceLow`, `priceHigh`, `side:"bid"|"ask"`, `qty` (peak dominant-side resting quantity), `notional` (peak quantity times cell midpoint), `meanQty`, `firstSeenMs`, `lastSeenMs`, `columns` and `forming`. Rows carry one-based `rank`; `forming` is true when the peak belongs to the current live bucket. Ranking is descending peak quantity, then ascending peak bucket, price low and side (bid first). Quantities are doubles from the CPU cell oracle, before display-code quantization. `meanQty` divides the sum of qualifying quantities by all recorded columns in the period (the retired legacy capture's semantics; HeatmapCellQueryTests checks it against a page oracle); `columns` counts qualifying appearances. Zero/invalid cells and quantities below `minQty` are excluded. These values describe aggregated resting size, not individual orders or proof of uninterrupted persistence.

A selection-epoch change during the scan returns `409 selection_changed`. (The legacy renderer's projection and its `409 recording_required` went in S8a.)

## Live hand-off checks

Run `sentinel-gui` with the API enabled, then use:

With a recording server, wait until the heatmap settles, then check:

```sh
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?limit=3&minQty=1'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?startMs=1790593200000&endMs=1790596800000&priceMin=64000&priceMax=65000'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?limit=101'
curl -si 'http://127.0.0.1:17100/api/v1/heatmap/walls?startMs=2&endMs=1'
```

Use actual loaded time and price bounds from `/api/v1/viewport` for the third request. Check quantity order, validity gaps, `loadedRange`, coverage counts and the source band tick against the visible heatmap. A large time range can return `422 scan_limit`; narrowing it should succeed.

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

Book prices are dense-book bucket starts on the active product's server-published tick, not individual exchange price levels; sizes are the bucket totals rounded to 1e-8. `/state` currently exposes the configured default in `serverConfig.orderbook.tickSize`, which is not the active product tick for non-BTC books. The DOM header displays the active tick; `/book` does not yet expose it separately. Best bid and best ask are bucket prices too, so they can share a bucket and `spread` can read 0 when the real spread is under one tick.

Control (`POST`) and `GET /api/v1/operations/<id>` responses use the standard envelope: read `operationId`, `status`, `viewportVersion` and `frameId` under `data`. The screenshot response keeps its legacy flat shape.

Walls are price levels, not minutes: each (price cell, side) appears once with `qty` = its peak aggregated resting size in the range (`bucketStartMs` = when it peaked), `meanQty` = mean over recorded columns in the range (absent = 0), `firstSeenMs`/`lastSeenMs`, and `columns` = recorded minutes it was present. Use `priceMin`/`priceMax` to look away from the mid, where coarse cells otherwise sum the dense book near the price. The scan budget counts recorded columns only.


### Subscription refusal diagnostics

`GET /api/v1/state` adds `data.lastSubscriptionRefusal`: `null` until this GUI receives a
server connection-cap refusal, then `{ "symbol": "ETH-USD", "maxConnections": 8,
"message": "Cannot subscribe ..." }`. An empty refusal symbol is resolved to the
single pending switch. This is the most recent refusal in this GUI session, retained
across retries/reconnects as a diagnostic, not the current connection state.
`connected` still describes the GUI-to-server socket. The server-provided message
appears in the status bar for the pending switch or a held-symbol reconnect failure;
a superseded refusal stays diagnostic only. Free another GUI-only product slot and retry the
subscription; pinned products do not consume these slots.
