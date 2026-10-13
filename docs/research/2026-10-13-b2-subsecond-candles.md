# B2: sub-second candles, tfMs end to end (plan, 2026-10-13)

Status: plan for owner sign-off. Base `main` 0c2745b. Written read-only; nothing else changed.
Owner decision (2026-10-13, option A): the candle interval is `tfMs` end to end; 25-500 ms get
real candles from trades, live and from the journal; a boundary that truly speaks seconds
refuses an unrepresentable interval with a logged reason; ranges the journal does not cover
show no candles. Line numbers are from 0c2745b and may drift; search by symbol.

## 1. Where the interval crosses a unit or type boundary today

GUI (ms) -> client (s) -> wire (s) -> server (s/ms) -> journal/REST -> back (s) -> GUI (ms).

| # | Site | Unit in | Unit out | Rounding |
|---|---|---|---|---|
| 1 | `TimeAxisModel.cpp:5-11` TIME_STEPS (25 ms .. 1 d) | ms | ms | none |
| 2 | `MainWindowGpu.cpp:126-139` `timeframeMsFromLabel` (toolbar: 1s, 1m, 5m, 15m, 1h, 4h, 1D only) | label | ms | none |
| 3 | `ChartViewController` session `timeframeMs`; `HeatmapGpuLayer::setTimeframeMs` (`UnifiedGridRenderer.Init.cpp:23,77`) | ms | ms | none |
| 4 | `PriceFitter.cpp:35,95,238`, `PriceCarryPolicy.cpp:49`: `max(1,(tf+500)/1000)` into `ICandleSource` | ms | s | round-half-up; 1500 -> 2 |
| 5 | `PriceFitter::candlesDirty(symbol, timeframeSec, ...)` (`PriceFitter.cpp:233-238`) wired at `UnifiedGridRenderer.cpp:360-362` | s | s | drops if != rounded tf |
| 6 | `DepthChartView.qml:103` `timeframeSec: Math.max(1, Math.round(timeframeMs/1000))` into `CandlestickOverlayItem` | ms | s | round; 100 -> 1, 1500 -> 2 |
| 7 | `CandlestickOverlayItem.cpp:375-376` `expectedTfMs = m_timeframeSec*1000; cadenceMatches = (frame.activeTimeframeMs == expectedTfMs)` | s | ms | mismatch at 100 ms: draws raw 1 s bars (`:468`) |
| 8 | `MainWindowGpu.cpp:1076` `requestCandleHistoryForSymbol`: `max(1,(tf+999)/1000)` | ms | s | ceil; 1500 -> 2, 100 -> 1 |
| 9 | `CandleSeriesBuffer` `SeriesKey{symbol, timeframeSec}` (`CandleSeriesBuffer.hpp:74-82`), `capacityFor(timeframeSec)` (`.cpp:13-15`) | s | s | key space is seconds |
| 10 | `CandleBackfillState.hpp:21-44,64,84` pages in `startSec/endSec/tfSec` | s | s | `tfMs = tfSec*1000` |
| 11 | `RemoteGridDataSource.cpp:273-315` `setCandleHistoryViewport(symbol, timeframeSec, ...)`, `requestCandleHistory(..., timeframeSec, ...)`; `IGridDataSource.hpp:43` | s | s | none |
| 12 | Wire `candle_history_request.timeframe_sec` (`SentinelStreamClient.cpp:637`); chunk and bar messages carry `timeframe_sec` (`SentinelStreamClient.cpp:980,1017`; server `:966,1026,1359`) | s | s | schema `kCandleSchemaVersion = 1` (`SentinelStreamProtocol.hpp:11`) |
| 13 | Server `candle_history_request` (`SentinelStreamServer.cpp:908-1046`): `timeframeSec==1` -> memory; else `timeframeMs = sec*1000`, must be a configured minute multiple (`:991-996`) else error `unsupported timeframe_sec`; REST path `fetchClosedCandleHistory(sec)` | s | ms/s | 1 s and minute multiples only |
| 14 | Server live `on_bar_updated` / `on_bar_closed` (`SentinelStreamServer.cpp:1312-1317,1366-1370`): `tfSec = timeframeMs/1000`, key `sym|tfSec`, wire `timeframe_sec` | ms | s | integer division: a sub-second tf would become 0 and all collide on one key |
| 15 | `TimeframeAggregator` (`TimeframeAggregator.cpp:12-26,135-143`): accepts only 1000 ms and minute multiples, warns and drops others; builds 1 s and 1 m from trades, coarser by rollup | ms | ms | config filter |
| 16 | `CoinbaseRestClient::granularityFromSeconds` (`CoinbaseRestClient.cpp:335`), `kCandleGranularitiesSec` (`TradeOverlayPublisher.hpp:51`) | s | REST name | true seconds boundary |
| 17 | Agent API candle query: `AgentApiCodec.cpp:251-252` rejects `timeframeMs % 1000 != 0`; `AgentApiSnapshots.cpp:22` `timeframeMs/1000` into the buffer | ms | s | refuse / truncate |
| 18 | Footprint and TPO history already use `timeframe_ms` on the wire (`SentinelStreamServer.cpp:428`, `RemoteGridDataSource.cpp:266-270`) | ms | ms | precedent: the wire can speak ms |

Trade timestamps carry Coinbase `time` parsed to microseconds (`MessageDispatcher.hpp:39-41`,
`Cpp20Utils.hpp:146`); candles bucket on `ms` (`TimeframeAggregator.cpp:28-32,97-100`). A 25 ms
bucket has real data; it is not a resampled 1 s bar.

## 2. Where live candles are built

- Server only. `ServerDataModel::onTrade` (`ServerDataModel.cpp:427-434`, model thread via
  `SentinelServerApp.cpp:154`) feeds `TimeframeAggregator::onTrade` (`:97-108`): one 1 s bucket and
  one 1 m bucket per trade; coarser bars roll up from closed minutes (`:46-71`). Captured products
  close on their journal clock (`advance`, `kJournalCandleLagMs = 1000`, `ServerDataModel.hpp:89`),
  carrying up to 300 quiet buckets per advance (`:150-170`). Closed bars: 10,000 per (product, tf) in
  RAM (`:40`), so the 1 s series covers about 2.8 h.
- Each session receives every `barUpdated` for every tf of a subscribed product, gated by
  `should_emit_update` (`SentinelStreamServer.cpp:1280-1310`; fast thresholds when `tfSec <= 1`).
  The client keeps only the selected (symbol, tf) (`RemoteGridDataSource.cpp:516`).
- The GUI already receives every trade of the subscribed symbol (`on_trade`,
  `SentinelStreamServer.cpp:1210-1221`; `IGridDataSource::tradeReceived`, `RemoteGridDataSource.cpp:86`;
  consumers `MainWindowGpu.cpp:553,1269,1286`, `OrderBookDock.cpp:89`).
- The GUI has no candle builder. `DataProcessor` has no candle or timeframe code (grep: none).
- A 25-500 ms bucket can be built from the data already flowing, live and recorded, without new
  data: the aggregator's `bucketStart`/`updateTradeBar` are interval-agnostic; the server's per-product
  trade tape `m_recentFootprintTrades` (`ServerDataModel.hpp:34-40,139-140`) holds ms-stamped trades for
  `clamp(2*maxTf*max(1024,retentionColumns), 5 min, 24 h)` = 24 h on the current config
  (`ServerDataModel.cpp:106-113`, `config/server_config.yaml:5`; documented in `docs/MARKETDATA.md:443-445`).

## 3. History granularities and a sub-second request

Served today (`SentinelStreamServer.cpp:932-1046`): `timeframe_sec == 1` from aggregator RAM
(10,000 bars, paged by `end_time_sec`); configured minute multiples from Coinbase REST through
`CandleHistoryCache` (`CoinbaseRestClient.cpp:356-499`; native granularities 1m..1d, others rolled up
from the largest native divisor; `docs/MARKETDATA.md:143-145`). Anything else: error
`unsupported timeframe_sec`; the client then retries after a 60 s floor
(`RemoteGridDataSource.cpp:82`, `CandleBackfillState.hpp:15`). REST is a true seconds boundary.

Readers available for a sub-second request, no format change:
- Tape: `collectOverlayTrades` copies a half-open window under `m_footprintTradeMutex`
  (`ServerDataModel.cpp:374-399`). It scans the whole deque linearly (`:386`) while holding the lock
  that `onTrade` takes per trade (`:441`). Fine for the overlay's bounded windows; for candles the
  copy should stop at the window end (the deque is append-ordered by arrival) or use a bounded scan.
- Journal: `JournalTradeEventSource(root, product, fromMs, toMs)` (`MarketEventSource.hpp:47-52`,
  `.cpp:120-156,161-215`): picks the hourly segment whose `opened_system_ns <= fromMs`
  (`:131-135`; segments rotate hourly, `RawCapture.cpp:364,389`), then decodes forward, JSON-parsing
  only frames that contain `market_trades` (`:194-195`). No block-level time seek yet, although
  `BlockIndex.firstSystemNs/lastSystemNs` (`RawCapture.hpp:66-74`) and `RecordReader(path, ..., startBlock)`
  (`:167-168`) allow one as a reader-only change. Trade IDs, ms time and sides are preserved
  (`docs/MARKETDATA.md:449-451`). The server already reads its own journal (ShadowRoller), so an
  in-process reader does not contend with the recorder's locks the way a second server would.

Cost per request (inferred where marked):
- Tape: 350 bars x 100 ms = 35 s of trades; at a peak of ~50 trades/s per product (assumption; the
  code and metrics config expose no trades/s figure; `docs/research/2026-10-one-world-pipeline.md:57,226`
  gives 20-91 journal records/s for 7 products, most of them L2) that is under 2,000 samples, well
  under 1 ms. The linear deque scan (`:386`) is the real cost: a day of BTC trades (inferred
  ~0.5-1 M) per request under the model-thread lock, so bound it (see slice 2).
- Journal: one hourly segment is at most ~75 k records for BTC (1.8 M records/day,
  `2026-10-one-world-pipeline.md:436,808`; ~33 k records/s decode) = 2-3 s worst case per request,
  less with the `market_trades` string filter. It runs on the 8-worker history pool
  (`kMaxPendingHistoryTasks = 8`, `SentinelStreamServer.hpp:123`; `kHistoryWorkerCount`, `.cpp:1718`)
  which chunks, candle and TPO history share. Acceptable for a scroll into yesterday; a block-level
  time seek cuts it to well under 1 s (slice 5, optional).
- Journal coverage starts 2026-10-06 (`config/server_config.yaml:46-47`). Earlier ranges: empty
  chunk, no retry (today an empty reply drives the `emptyLookbackSec` floor,
  `CandleBackfillState.hpp:16-17`), no REST.
- Wire change: yes, `timeframe_sec` -> `timeframe_ms` on four candle messages (section 4). Recording
  or on-disk format change: none. Server config change: none (`timeframes_ms` stays the live set).

## 4. Hot-path impact (AGENTS 5)

- Server `onTrade` and `TimeframeAggregator::onTrade` (recorder, always-on): untouched in the
  recommended design. Building every sub-second tf on the server would add 5-10 bucket updates and
  `barUpdated` emits per trade per product, each queued per session (`SentinelStreamServer.cpp:785-799`,
  one `std::function` and one JSON dump per passing gate): rejected (section 5).
- Server history worker: tape copy and candle build per request, bounded by `limit` (350 bars).
- GUI data thread: one bucket update per trade for the selected (symbol, tf); `CandleSeriesBuffer::applyUpdate`
  emits `candlesDirty` per update (`CandleSeriesBuffer.cpp:111`) which drives `PriceFitter::candlesDirty`
  and a refit; that is the same cadence as today's server bar updates and the 1 s gate. Coalesce to at
  most one emit per frame (`sLog_*N` or a 16 ms timer) if a replay or burst shows refit storms.
- Render thread: `CandlestickOverlayItem::updatePaintNode` filters bars and calls `buildContinuousBars`
  per frame (`.cpp:455-468`), bounded by `gridWidth` columns. No new allocation: `m_continuousCandles`
  is reused. `PriceFitter::visibleBars` uses the reused `bars_` (`PriceFitter.cpp:92-95`).
- GUI buffer memory: `capacityFor` must take ms; keep 60,000 bars for tf <= 1,000 ms (25 min at 25 ms).

## 5. Design (one recommendation)

`tfMs` everywhere; the wire speaks ms; the server builds sub-minute history from its trade tape then
its journal; the GUI builds the live sub-minute bar from the trade stream it already receives.

Rules:
1. Interval set. Any `tfMs > 0` with `60000 % tfMs == 0` is a candle interval (25 .. 30000 ms and
   60000). Minute multiples as today. `1500` qualifies; `7` or `700` do not (refused, logged).
2. Server `candle_history_request{timeframe_ms, end_time_ms, limit}`:
   `tfMs % 60000 == 0` and configured -> REST path (the only seconds boundary; `tfMs/1000` is exact).
   `tfMs == 1000` -> aggregator RAM as today. Other qualifying `tfMs` -> tape window if the window
   start is inside the tape, else `JournalTradeEventSource` for the window, built with a shared
   `TradeCandleBuilder` (pure function, `libs/core/servermodel/`); empty chunk when the journal does not
   cover the range. Anything else -> error `unsupported timeframe_ms` and a `sLog_Warning` with the
   reason; the client stops retrying that (symbol, tf).
3. Live. The server keeps streaming the tfs it aggregates (`timeframes_ms`). For a tf the server does
   not stream (the client knows the list, `SentinelStreamClientParseHelpers.cpp:48-53`) the data
   source feeds `tradeReceived` into a `TradeCandleBuilder` instance for the selected (symbol, tf) and
   applies the open bar to `CandleSeriesBuffer` through the existing live-ownership path. History
   never overwrites a live-owned bucket (`docs/MARKETDATA.md:147` contract unchanged).
4. GUI units: `CandleSeriesBuffer`, `CandleBackfillState`, `RemoteGridDataSource`, `CandlestickOverlayItem`
   (`timeframeMs` property), `DepthChartView.qml`, `PriceFitter`, `PriceCarryPolicy`, `candlesDirty`
   all carry `tfMs`. Sites 4-11 and 17 lose their divisions. `AgentApiCodec.cpp:251` whole-seconds
   reject is deleted.

Rejected:
- Server-side live aggregation of every sub-minute tf: recorder hot path, per-trade fan-out to every
  session, 10,000-bar RAM per (product, tf) (250 s at 25 ms), and a deploy for a chart feature.
- Server-side on-demand aggregation (per-session tf subscription): correct but adds subscription
  state and refcounts to the recorder; the GUI already has the trades.
- GUI-only (build history from a client-side journal reader): the GUI must not read `/Volumes/T7`.
- Option B (refuse sub-second history): not needed; the tape and journal readers exist.
- Rounding at any site: forbidden by the decision.

## 6. Slices, landing order

Each is its own branch, 1 writer + 1 reviewer. Hot files: `MainWindowGpu.cpp` (slice 3),
server files (slice 2). `DataProcessor.cpp`: none.

1. B2-1 `TradeCandleBuilder` (core, low risk). Files: `libs/core/servermodel/TradeCandleBuilder.{hpp,cpp}`,
   `tests/servermodel/test_trade_candle_builder.cpp`. Pure: `build(trades, tfMs, fromMs, toMs)` and an
   incremental `add(trade)` / `closeThrough(ms)`; same bucket rule as `TimeframeAggregator::bucketStart`.
   Tests that fail without it: 100 ms buckets from ms-stamped trades; 1500 ms bucket edges at
   0, 1500, 3000; a trade at exactly a boundary opens the next bar; stale trade ignored.
2. B2-2 wire + server (high risk: recorder; reviewer per AGENTS 10, deploy by the conductor).
   Files: `SentinelStreamProtocol.hpp` (`kCandleSchemaVersion = 2`), `SentinelStreamServer.cpp`,
   `SentinelStreamClient.{hpp,cpp}`, `SentinelStreamClientParseHelpers.cpp`, `ServerDataModel.{hpp,cpp}`
   (bounded tape window accessor; `onTrade` untouched), `docs/MARKETDATA.md`.
   `timeframe_sec` deleted from the four candle messages, `timeframe_ms` added, `end_time_ms` replaces
   `end_time_sec`. Tests that fail without it (`tests/marketdata/test_protocol_validation.cpp`,
   `tests/servermodel/`): request 100 ms -> chunk of 100 ms bars from a seeded tape; 1500 ms ->
   1500 ms bars, zero REST calls; 700 ms -> `unsupported timeframe_ms` and a warning line; 60000 ms
   -> REST with granularity `ONE_MINUTE`; a range before the journal start -> empty chunk, no error;
   bar updates carry `timeframe_ms`. Scenario evidence for the hand-off: restart and reconnect with a
   100 ms chart open; the tape window copy stays under 1 ms at a day of trades.
3. B2-3 GUI units + history (medium; touches `MainWindowGpu.cpp:1073-1082`, chart core, QML, UGR
   wiring line `UnifiedGridRenderer.cpp:360`). Files: `CandleSeriesBuffer.{hpp,cpp}`,
   `CandleBackfillState.hpp`, `RemoteGridDataSource.{hpp,cpp}`, `IGridDataSource.hpp`,
   `CandleBufferSource.hpp`, `ChartPorts.hpp`, `PriceFitter.cpp`, `PriceCarryPolicy.cpp`,
   `CandlestickOverlayItem.{hpp,cpp}`, `DepthChartView.qml`, `AgentApiSnapshots.cpp`, `AgentApiCodec.cpp`.
   Tests that fail without it: `test_price_fitter.cpp` `candlesDirty("TEST", 100, ...)` accepted at
   tf 100 and `candlesDirty("TEST", 1000, ...)` ignored at tf 1500; `test_CandleBackfillState.cpp`
   pages 350 x 100 ms with ms edges; `test_CandleDataSource.cpp` series keyed by 1500 ms;
   `test_MainWindowSymbolLifecycle.cpp` history request carries `timeframe_ms = 100` at tf 100 and
   `1500` at tf 1500 (no ceil); Agent API candle query accepts `timeframeMs = 100`. Chart-health must
   pass; no golden re-bless expected (candle geometry at 1 s is unchanged); if the 100 ms CPU trace
   changes, the hand-off names it.
4. B2-4 GUI live builder (medium). Files: `RemoteGridDataSource.{hpp,cpp}` (+ a small
   `LiveCandleFeed` in `libs/gui/datasources/`), `docs/MARKETDATA.md`. Tests that fail without it:
   with the served list `{1000, 60000}` and tf 100, three trades at 10, 120, 130 ms produce two open
   bars through `applyUpdate`; switching to tf 1000 stops the local builder and keeps server bars;
   a history bar never replaces a live-owned bucket. GUI run on the host with `SENTINEL_PROBES=candles`
   at 100 ms and 1500 ms for the hand-off screenshot (`heatmap` target).
5. B2-5 optional: block-level time seek in `JournalTradeEventSource` from `BlockIndex.firstSystemNs`
   (reader only, no format change). Only if slice 2's journal timing exceeds ~1 s per request.

B2 row acceptance (plan row B2): tf = 100 ms and 1,500 ms at all three former sites: `PriceFitter`
passes 100/1500 unchanged (slice 3 test), `requestCandleHistoryForSymbol` sends 100/1500 (slice 3),
QML binds `timeframeMs` directly and the overlay's `cadenceMatches` holds at 100 ms (slice 3; a CPU
golden or a GUI probe line `candles.empty` absent at 100 ms).

## 7. Owner sign-off questions (recommended default first)

1. Live sub-second bars built in the GUI from the trade stream it already receives (default), or on the
   server for every configured tf (adds per-trade fan-out on the recorder and a deploy per change)?
2. Interval set at the history boundary: every `tfMs` that divides 60,000 ms, so 1,500 ms and 2-30 s
   get trade-built candles too (default); or only the five axis steps 25-500 ms plus 1 s and minute
   multiples?
3. Journal-backed history for ranges older than the 24 h tape in B2 (default; one hourly segment per
   request, 2-3 s worst case on the shared history pool), or tape only until a later slice (older
   ranges show no candles)?

## 8. Open risks

- The wire change is not backward compatible by rule: GUI and recorder must be deployed together;
  an old GUI against the new server gets `unsupported` errors on every candle request.
- Tape scan under the model-thread lock (`ServerDataModel.cpp:386`): a careless window copy stalls
  `onTrade`. Slice 2 must bound it and measure.
- Live/history seam at the GUI: the first local bar starts at the first trade seen; the history reply
  for the same bucket must lose to the live-owned bar; a reconnect restarts the local builder and
  queues one refresh (existing contract, `docs/MARKETDATA.md:147`).
- `closeElapsed` carries at most 300 quiet buckets per advance (`TimeframeAggregator.cpp:159-168`):
  irrelevant for the GUI builder, but if the server-side option is chosen, 25 ms x 300 = 7.5 s.
- History pool contention: journal reads share 8 workers with chunks and TPO; a fast scroll through
  several days could queue 2-3 s jobs. Cap one journal candle job in flight per session.
- Coverage before 2026-10-06 is empty by decision; the chart shows the heatmap with no candles there.
- STATUS "Next" item 3 (1 m forever, 1 s for 5 days from the journal) is not in the code: the
  aggregator holds 10,000 bars per tf in RAM and minute history beyond that comes from REST. B2 does
  not implement that item; it only makes sub-minute history independent of it.

## 9. Surprises and doc corrections

- At 100 ms the chart today shows 1 s candles on a 100 ms grid (`cadenceMatches` false, raw bars drawn,
  `CandlestickOverlayItem.cpp:375-376,468`), not nothing. At 1,500 ms it shows nothing: round -> 2 s,
  server refuses, 60 s retry loop.
- A sub-second tf cannot ride the current live wire at all: `tfSec = timeframeMs/1000` is 0
  (`SentinelStreamServer.cpp:1316`), so every sub-second tf would share one gate key and one wire value.
- The server already retains 24 h of ms-stamped trades per product in RAM; no recording change is
  needed for 24 h of sub-second history. `JournalTradeEventSource` already reads older trades.
- `TimeframeAggregator` silently drops sub-second entries from `timeframes_ms` with a warning
  (`TimeframeAggregator.cpp:18-23`); adding 100 to the config does nothing today.
- The Agent API candle query (`AgentApiCodec.cpp:251`) is a seconds boundary that does not need to be one.
