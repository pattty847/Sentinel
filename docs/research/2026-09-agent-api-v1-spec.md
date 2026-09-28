# Sentinel Agent API v1 — spec

Author: Lt. Astra (gpt-6-astra, read-only design pass), 2026-09-28. Reviewed by the orchestrator.

Orchestrator notes:
- Build order is the four slices at the end. Slice 1 first.
- Linked candle/heatmap timeframes are a v1 simplification only. INV-004 (independent timeframes) stays the target.
- Risk 3 (CandlestickOverlayItem reads CandleSeriesBuffer in updatePaintNode): a policy breach of INV-051 but not a data race, since Qt runs updatePaintNode during sync with the GUI thread blocked and the buffer is GUI-thread only. Low priority; fix when the candle look-and-feel work touches that file.
- Slice 3 (walls) waits for recording v2, so walls come from absolute sizes. Slice 4 (controls) goes first.

---

Sentinel Agent API v1 should expose the **active chart and locally loaded evidence**, with explicit freshness, coverage, and render acknowledgements. It should not become a second market-data server. This is a read-only specification; no files were changed or live rendering verified.

The current implementation establishes three constraints: `DepthChartView.qml` derives candle timeframe from `UnifiedGridRenderer::timeframeMs`; `DataProcessor` owns heatmap columns on its worker; and `viewportVersion` records viewport changes, not completed rendering.

**Contract.** Use `/api/v1`, UTC epoch milliseconds, half-open time ranges `[startMs,endMs)`, numeric prices/quantities, and decimal-string counters. Reads default to the active symbol; a different symbol returns `409`. Reads never trigger history fetching.

All JSON responses use:

```json
{"ok":true,"meta":{"sessionId":"s1","symbol":"BTC-USD","selectionEpoch":"3","observedAtMs":1790596800000,"source":"gui-cache","stale":false,"coverage":"partial","truncated":false},"data":{}}
```

`sessionId` changes on GUI restart; `selectionEpoch` changes on symbol/timeframe/reconnection. Coverage is `complete|partial|unknown`, separately from truncation. Errors use `{"ok":false,"error":{"code":"invalid_range","message":"startMs must precede endMs"}}`.

The following examples show `data`; omitted metadata remains mandatory.

| Method/path | Parameters or JSON body | Example response |
|---|---|---|
| `GET /state` | None | `{"connected":true,"serverConfigReady":true,"server":{"host":"127.0.0.1","port":8080},"serverConfig":{"defaultSymbols":["BTC-USD"],"heatmap":{"configuredTimeframesMs":[1000,60000],"servedTimeframesMs":[60000],"activeTimeframeMs":60000,"gridWidth":5120,"gridHeight":2048},"orderbook":{"tickSize":0.1,"bandPct":0.3}},"layers":{"heatmap":true,"candles":true,"footprint":false,"tpo":false,"volumeProfile":false}}` |
| `GET /viewport` | None | `{"startMs":1790593200000,"endMs":1790596800000,"priceMin":64000,"priceMax":65000,"heatmapTimeframeMs":60000,"candleTimeframeMs":60000,"followLive":false,"viewportVersion":"42","widthPx":1200,"heightPx":600,"zoom":{"msPerPx":3000,"pricePerPx":1.666667}}` |
| `GET /candles` | Required `startMs,endMs,timeframeMs`; `limit=500`, maximum 2,000 | `{"bars":[{"startMs":1790593200000,"endMs":1790593260000,"open":64100,"high":64200,"low":64080,"close":64180,"volume":12.5,"closed":true,"seq":"27"}],"nextStartMs":null}` |
| `GET /book` | `levels=20`, maximum 200 per side | `{"bestBid":64100,"bestAsk":64100.1,"spread":0.1,"bids":[[64100,2.4]],"asks":[[64100.1,1.8]],"bandLimited":true,"band":[45000,83000],"receivedAtMs":1790596800000}` |
| `GET /heatmap/walls` | Optional time/price bounds, default loaded window; `minQty=0,limit=20`, maximum 100 | `{"basis":"source-column-twap","loadedRange":[1790593200000,1790596800000],"recordedColumns":60,"missingColumns":0,"walls":[{"bucketStartMs":1790593200000,"priceLow":64000,"priceHigh":64000.1,"side":"bid","qty":18.2,"notional":1164800,"forming":false}]}` |
| `GET /trades` | `windowMs=60000`, maximum 900000; `limit=100`, maximum 1,000 | `{"timeBasis":"received","trades":[{"receivedAtMs":1790596800000,"eventTimeMs":null,"id":null,"side":"buy","price":64100,"qty":0.2}],"summary":{"count":1,"buyQty":0.2,"sellQty":0,"unknownQty":0,"deltaQty":0.2,"vwap":64100},"retentionLimited":false}` |
| `POST /symbol` | `{"symbol":"ETH-USD"}` | `{"operationId":"o7","status":"applied","symbol":"ETH-USD"}` |
| `POST /timeframe` | `{"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}` | `{"operationId":"o8","status":"applied","linked":true,"heatmapTimeframeMs":60000,"candleTimeframeMs":60000}` |
| `POST /viewport` | Paired time and/or price bounds; optional `followLive` | `{"operationId":"o9","status":"applied","viewportVersion":"43","followLive":false}` |
| `POST /layers` | Partial boolean map, e.g. `{"heatmap":true,"candles":false}` | `{"operationId":"o10","status":"applied","layers":{"heatmap":true,"candles":false,"footprint":false,"tpo":false,"volumeProfile":false}}` |
| `GET /operations/o9` | `waitMs=0`, maximum 5000 | `{"status":"rendered","viewportVersion":"43","frameId":"912","dataState":"partial","pending":["heatmap-history"]}` |
| `GET /screenshot` | Existing `name,target=main|heatmap|lab`; add `afterOperation,waitMs` | Preserve existing `{"ok":true,"path":"./screenshots/review.png","target":"heatmap"}`; add captured `frameId`, epoch and viewport metadata. |

Serve screenshot under `/api/v1/screenshot` too. An unavailable best price is `null`.

`/state` additionally includes advertised candle-gating settings and per-source last-received timestamps. Return unavailable server capabilities as `null`, not inferred defaults. `servedTimeframesMs` is authoritative; configured candidates are not availability.

For v1, timeframe selection remains **linked**: either field sets both; unequal values return `422`. Reject advertised-unserved heatmap timeframes. Unknown availability on older servers remains explicitly unknown.

Explicit time bounds default `followLive` to false; reject bounds combined with `followLive:true`. Follow-only requests preserve span. Omitted price bounds preserve the current range.

Walls are deterministic top cells ranked by decoded quantity, then time/price/side—not claims about individual orders. Decode little-endian liquidity as `raw * liquidityScale`, using source bands and signed intensity for side. Report missing liquidity separately from zero. Source columns avoid display resampling’s independent intensity/liquidity maxima. TWAP cells cannot establish continuous persistence or current resting size.

**Ownership and execution.** Refactor `GuiApiServer` transport into a dedicated I/O thread; create/listen its sockets there. Queue typed requests to a GUI-owned controller, returning immutable snapshots for off-thread JSON encoding. Never block the GUI waiting for another thread.

- State, viewport, controls and readiness belong to the GUI thread. Read `GuiConfigStore`, renderer getters and `ChartModeController::candlesEnabled()`.
- Candles use `CandleSeriesBuffer::getVisibleSlice()` through a new bounded accessor; normalize its currently inclusive upper boundary to the API’s half-open contract.
- Book snapshots use GUI-only `RemoteGridDataSource::getDirectLiveOrderBook()` and `LiveOrderBook::captureDenseNonZero()`. Copy values, never retain spans. Its scan can traverse the entire dense band: add a scan budget or bounded maintained index before promising latency.
- Tape uses a preallocated GUI ring fed by `IGridDataSource::tradeReceived`, retaining at most 10,000 trades/15 minutes. Summaries cover retained matches, not just returned rows. Current `handleTradeMessage()` discards wire `time`; report receive time honestly until corrected.
- Walls queue into `DataProcessor`, capturing immutable `ColumnWindow` source columns. Analyze on one bounded background worker; cap retained bytes and examined cells, rejecting excessive requests.
- Screenshot capture remains GUI-owned; PNG encoding/writing moves off-thread. `grabWindow()` can stall: serialize and rate-limit captures. Zero rendering impact cannot honestly be guaranteed.

Reuse extracted, nonmodal helpers from `MainWindowGPU::onSubscribe()`, toolbar timeframe/layer handlers, and `requestConfiguredHistoryForSymbol()`. Viewport changes must call `UnifiedGridRenderer::setViewport()` → `GridViewState::setViewport()`; follow uses `enableAutoScroll()`.

**Render acknowledgement.** Add a control revision plus selection epoch to GUI-produced snapshots. Carry these, viewport version and consumed data generations through frame synchronization; publish completion after that frame renders. INV-051 forbids render callbacks consulting live QObject graphs. Use fixed-size acknowledgements, without per-frame allocations or new geometry churn.

Operations distinguish `applied`, `rendered`, `superseded`, and `failed`; history state is separate. Async long-poll expiry returns current pending state. A guarded screenshot times out explicitly rather than capturing early. Later controls invalidate its operation; live data may advance. This guarantees ordering, not identical pixels or complete history. `historyRequestInFlight=false` alone is insufficient: the existing five-second timeout also clears it.

**Safety and implementation.** Bind only `127.0.0.1`; preserve `api_port=0`. Allowlist routes, fields, symbols and targets; require JSON controls, finite ordered bounds, valid integer timeframes and bounded arithmetic. Reject browser cross-origin requests and unexpected Host headers. Expose no trade/algo commands, including paper trading, filesystem paths from config, or arbitrary invocation.

Limit headers/body to 8/16 KiB, JSON responses to 1 MiB, concurrent requests to eight, screenshots to one/second; enforce read/write deadlines. Restrict screenshot names and prevent traversal/symlink escapes.

Add `libs/gui/mainwindow/AgentApi{Types,Codec,Controller}.*`; modify `GuiApiServer.*`, `MainWindowGpu.*`, datasource accessors, `DataProcessor.*`, `HeatmapColumnWindow.*`, and renderer/frame snapshot files. Register in `libs/gui/CMakeLists.txt`. Core changes, if needed, stay confined to protocol parsing/error DTOs; apps remain unchanged.

**Four mergeable builder slices:**

1. Transport, DTOs, validation, state/viewport reads. Headless parser tests cover fragmented bodies, limits and invalid inputs.
2. Candle/book/tape snapshots. Fixture tests cover boundaries, ordering, stale epochs, truncation and summary arithmetic.
3. Walls. Extend `tests/render/test_HeatmapColumnWindow.cpp`; test scaling, sides, missing/zero columns, band changes, budgets and deterministic ranking.
4. Controls, operation tracking and screenshots. Fake-thread tests cover supersession, delayed history, timeout and frame acknowledgements; document in `docs/AGENT_API.md`.

Each slice builds affected targets with `mac-clang` and runs targeted CTest cases. The orchestrator must verify actual layer changes, pan/follow, reconnects, hidden-window timeouts and screenshot ordering, inspect run logs, and compare frame profiles under API load.

**Ranked owner decisions/risks:**

1. Accept linked timeframes for v1; independent candles require mapping changes.
2. Define readiness expectations: protocol history replies lack operation IDs, and server errors currently only log. Strong history completion needs correlation/error plumbing.
3. Existing `CandlestickOverlayItem::updatePaintNode()` reads `CandleSeriesBuffer` directly. Replace that with GUI snapshots before relying on the new snapshot guarantee.
4. Receive-time tape, band-limited books and quantized TWAP are incomplete evidence. Preserve these limitations in CopeNet’s stored observations.