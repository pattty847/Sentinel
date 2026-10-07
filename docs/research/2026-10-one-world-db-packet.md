# One world slice D-b: the journal feeds the server (plan, 2026-10-06)

Read-only packet by Claude Fable, verified against `main` @ `985ceea`. The conductor accepted it with the decisions in
section 8. Parent plan: `docs/research/2026-10-one-world-pipeline.md`.

## 0. Findings that change the plan

1. **Trades are already captured and parsed; no recording-format change.** Capture journals `level2`, `market_trades` and `heartbeats` (`RawCapture.cpp:257`). `JournalFeed` already parses `market_trades` into `onTrade` (`JournalFeed.cpp:80-91`) and `TransportUp/Down` into `onConnection`. The roller discards both (`ShadowRoller.cpp:186-187`).
2. **Aggressor-side defect on the journal trade path.** The engine flips Coinbase's maker side to the aggressor side (`MarketDataCoreEngine.cpp:353-359`). `JournalFeed::onTrade` delivers the side unflipped (`JournalFeed.cpp:82-85`). D-b must flip once in `JournalFeed` and test it (A4). The current roller trade test only checks a timestamp.
3. **Product metadata does not need REST.** Every journal header carries the Coinbase product JSON as `product_metadata` (`CaptureApp.cpp:130-131`). `LiveSource::inventory()` already loads it. The server's `fetchProductMetadata` (`SentinelStreamServer.cpp:2325-2342`) becomes unused when serving.
4. **REST stays, for candles only.** It serves `candle_history_request` and the TPO candle backfill. REST is request/response, so "one Coinbase connection" means one WebSocket session per product, held by capture. The `Authenticator` stays for the REST JWT.
5. **The live model must not be fed from the history feed.** History is durable-only (INV-114) and runs about 1 s behind. The live book and tape need the provisional socket records, as the lead does. The tap is therefore a third consumer in `LiveSource`, beside the lead.
6. **No capture reconnect is needed for a book seed.** The history `JournalFeed` replays from the day anchor at every (re)start, so a worker-local raw book that follows it is complete at the tip.
   - It is emitted to the model as one synthesized snapshot, followed by provisional updates.
   - Retract and disconnect re-seed from the retained durable prefix. This replaces `recordingResnapshotRequested` -> `requestResnapshot` and the "wait for the next upstream snapshot" paths (`ServerDataModel.cpp:645-654, 817-824`).
   - `BookRecorder` exposes no levels, so the raw book is separate state.
7. **The `coinbase_latency` wire and `sentinel_mdc_ws_latency_ms` are dead ends.** The GUI slot is a no-op (`StatusBar.hpp:22`). Delete the server side and leave the client parse until slice E (INV-135).
8. **Stale line references in earlier docs.** Current locations:
   - `ServerDataModel.cpp`: book `:608-900`, trades `:578-606`, metadata `:341-376`.
   - `SentinelServerApp.cpp`: `:72-81`, `:105-171`, `:173-190`, `:192-237`.
9. **Already done:** `JournalFeed` callbacks; the stall monitor follows the roller workers; the roller serves recording and the live minute (D-a, INV-133). Capture has `sentinel_capture_feed_up` and its alert, so the server's `sentinel_mdc_connected` and alert A1b become redundant.
10. **Admission after D-b** is `rollerProducts()` (the 7 captured products). Any other product is refused with `InvalidProduct`, which the client already handles. `mdc.max_connections` and its gauges lose meaning.
11. **`TickBinaryLogger`** writes every trade and book message to `data/market/` on the main thread (`ServerDataModel.cpp:101, 580-582`). D-b2 removes the model's use of it; the class stays because `test_backtest_core.cpp:337` uses it (FM-204).
12. **Existing tests that bind the server to the engine (FM-201):**
    - `ServerFeedAdmissionTests`, including a source-text test of `m_marketDataCore->add`.
    - `ServerMetricsTests` (`sentinel_mdc_connected`).
    - `RecordingServerStop.SelfInvalidationIsHandedToTheMainThread`.
    - `ShadowRollerTests`, which compiles `SentinelServerApp.cpp`.
    - `MarketDataEngineReconnectTests`.

## 1. Scope and split (one writer at a time, serialized on the hot files)

### D-b1: the journal feeds the model (behaviour change, config-reversible)

**`ShadowRoller.{cpp,hpp}` and `ShadowConfig.hpp`:**
- A `ModelSink` (snapshot, updates, invalidate, trade, connection, metadata), installed by the app like `publisher`.
- A per-product worker-local raw book. It follows the history feed during catch-up and the provisional socket records at the tip.
- A synthesized snapshot at the tip, after every recovery, and on a re-seed request.
- `onTrade` with the aggressor flip; `onConnection`.
- A tip-age guard: with no socket record for 30 s, connection is false.
- Everything runs on the worker thread. Each sink call is a `Qt::QueuedConnection` hand-off.

**`JournalFeed.cpp`:** the aggressor flip.

**`SentinelServerApp.{cpp,hpp}`:** when the journal feeds the model:
- no `MarketDataFeeds` is constructed;
- the sinks are installed in `startShadow`;
- admission is `rollerProducts()`, and any other product gets `InvalidProduct`;
- no latency broadcast;
- the destructor order holds without the engine.

**`ServerDataModel.{cpp,hpp}`:**
- `m_feeds` = `rollerProducts()`, pinned;
- `onProductMetadata` accepts the journal header;
- a `requestReseed(symbol)` hook replaces the "await upstream snapshot" paths;
- `onMarketDataConnectionChanged` is fed by the sink.

Candles, TPO, the trade overlays and paper trading hang off `onTrade` and need no change.

**Tests:** `test_shadow.cpp`, `shadow_mutations.py`, `test_roller.cpp` and `test_server_metrics.cpp`. **Docs:** `ops/monitoring/README.md`.

**Not touched:** `SentinelStreamServer.*`, `MarketDataCoreEngine.cpp`, `MarketDataFeeds.*`, `DataProcessor.cpp`, `HeatmapTwapStreamer.cpp`, `libs/gui`, `BookRecorder.*`, `RecordingLive.*`.

### D-b2: the engine leaves the server (deletion, after D-b1 soaks 48 h)

**Delete:**
- `MarketDataFeeds` ownership and its bridge.
- The primary `BookRecorder` path and `recording.source`, plus `recordingResnapshotRequested`.
- The `m_logger` use.
- The `sentinel_mdc_*` gauges and alert A1b.
- `mdc.max_connections` and the WebSocket keys; keep `ssl_ca_bundle`.
- `broadcastCoinbaseLatency`, `requestBookProductMetadata` and `acquireGuiFeed/releaseGuiFeed`.

**Rewrite:** `ServerFeedAdmissionTests`, on top of the roller admission.

**Order:** this slice touches `SentinelStreamServer.cpp`, so it runs before S8b and slice E. After D-b2, rollback is binary-only.

**Roles:** writer Claude `opus` (native hosted-GUI evidence); reviewer `gpt-6-astra` at high (high risk).

## 2. Interfaces and invariants

- The `ShadowConfig` sink callbacks are not part of `configHash`; re-run D-a's A6.
- `LiveSource::controlOrRecord` and `discardProvisional`: one raw-book update and one queued hand-off per record, and no allocation beyond the record copy that is already made.
- The `ServerDataModel` slots stay on the main thread, and the signal shapes are unchanged.
- INV-114, INV-115, INV-126, INV-128 and INV-006 are unchanged.
- New invariant at landing: the server opens no Coinbase WebSocket. The live book, tape, candles and metadata come from the capture fan-out and journal; REST is used only for candle history.

## 3. Acceptance checks (each must fail without its change; one mutation each)

| Check | What must hold |
|---|---|
| A1 Tip seed | One synthesized snapshot at the tip, equal to a batch-replayed raw book, then provisional updates in order, each before its `durable` control. |
| A2 Recovery | Retract, disconnect and EOF each invalidate and then produce a snapshot equal to the durable state; no `resnapshot` control is sent to the fan-out. |
| A3 Metadata from the header | PEPE with 0 REST calls; `bookSnapshotBroadcast("ready")` with the `deriveGrid` tick. |
| A4 Aggressor | A journal `market_trades` record with `side: BUY` arrives as `AggressorSide::Sell`; footprint and `barUpdated` fire. |
| A5 App | With the journal feed on: no engine; PEPE-USD accepted, XRP-USD refused with `InvalidProduct`. With it off: today's suites pass unchanged. |
| A6 Connection | `TransportDown` gives "invalidated"; 30 s of silence gives connected=false; `TransportUp` plus a snapshot gives ready. |
| A7 Restart mid-minute | The seed arrives after catch-up; extra catch-up cost for 7 products is within 10% of D-a. |
| A8 Midnight | The raw book persists across the day rotation, with at most one re-seed and no "invalidated". |
| A9 | `configHash` is unchanged with the sink installed. |

**Targeted tests (FM-201):**
- `ShadowRollerTests`, `RollerTests`, `RollerLiveFixtureTests`
- `DeployMarkerTests`, `DeployRuntimeDryRun`
- `ServerFeedAdmissionTests`, `ServerMetricsTests`, `RecordingServerStopTests`
- `LiveBookTickTests`, `TimeframeAggregatorTests`, `TradeOverlayTests`
- `MarketDataEngineReconnectTests`, `MarketDataEngineConnectTimeoutTests`
- `CaptureApplicationTests`, `CaptureFanoutTests`
- `HeatmapChunkWireTests`, `RecordingLiveTests`

## 4. Scenario evidence (high risk)

- **Failure fixtures:** capture outage, fan-out disconnect/reconnect, ring miss and retract, plus a 30 s fake-clock outage. Each must go invalidate -> re-seed -> live, with history unchanged (strict `hmc2_diff` = 0).
- **Restart:** a real restart of the writer's own server, logging the time from start to a "ready" book per product.
- **Midnight:** synthetic now, plus one real midnight on the deployed binary.
- **PEPE:** the seed and updates below $0.005 reach the model, shown in an `orderBook` dock screenshot.
- **Hosted isolated GUI against the writer's own server instance:** temp hmc2 root, read-only fan-out client, never the owner's recorder or GUI.
  - 10 min before/after live age; the gate is p95 no worse than today + 50 ms.
  - `lastReceivedAtMs.book/trades/candles/footprint/tpo/volumeProfile` advancing.
  - Screenshots: `orderBook`, `heatmap` and `window` for BTC-USD and PEPE-USD.
  - `lsof -p <pid> -i :443` shows only transient REST connections.

## 5. Deploy runbook (conductor, owner at the Mac, not 23:55-00:05 UTC)

**Preconditions:**
- R2 is live and its 48 h soak is clean; `d10c56a` was deployed with R2; D-b1 has landed.
- The main checkout is clean on `main`.
- The D-a R2 checks hold again: all 14 mismatch series are 0, `lag_seconds` < 5, `fanout_clients` == 1, `ring_oldest_age` >= 55.
- The live-age baseline is captured; the rollback binary is present; the screen is unlocked; the build queue is idle.

**Steps:**
1. Build at `-j 2`, then run `deploy-runtime.sh server`. `Roller serving ready` must appear within 150 s.
2. First 5 min:
   - no `MarketDataFeeds` line;
   - the book is "ready" for BTC and PEPE within 60 s;
   - trades and candles are advancing;
   - capture `feed_up` is unchanged.
3. 10 min: the live-age gate. 1 h: the hourly comparison. 24 h: midnight. 48 h: then D-b2.

**Rollback triggers:**
- the deploy marker never appears;
- a book is not "ready" after 5 min;
- trades stall for 2 min while capture is up;
- p95 is above the gate;
- the mismatch count rises;
- `lag_seconds` > 60 for 5 min.

**Rollback:** set `recording.live_feed: engine`, or redeploy the previous binary (see decision 2 in section 8).

## 6. Risks (ranked)

1. **Single source:** a capture freeze (FM-127) now blanks the server's live book and tape. The tip-age guard surfaces it as "invalidated". This is owner question 2.
2. **Main-thread load:** 7 always-pinned live books plus their trades.
3. **Seed cost** at restart and recovery.
4. **Retracted provisional trades** reach the in-memory tape. This is rare; accepted and noted.
5. **Header metadata is as old as the capture run.**
6. **Two compiled paths** exist until D-b2.
7. **Hot files are shared with S8b and slice E:** serialize D-b1 -> D-b2 -> S8b/E.

## 7. Owner questions

1. Stop the legacy tick logs (`data/market`) in D-b2 and leave the existing files untouched? Default: yes.
2. Accept that a capture outage also blanks the server's live book and trades (one source, shown as invalidated)? Default: accept.
3. Products outside the 7 captured ones are refused with "invalid product", and adding a product stays capture-side work for later? Default: accept.

## 8. Conductor decisions (2026-10-06)

1. The D-b1/D-b2 split and the findings are accepted.
2. **Separate cutover switch.** D-b1 adds its own config key, independent of `recording.source`: `recording.live_feed: engine | journal` (writer may refine the name). The default is `engine`, and `journal` requires `recording.source: roller`. This keeps R2 (roller serves recording) and the D-b1 cutover as two separate, config-reversible steps. Without it, any binary built after D-b1 lands would switch both at R2. The key is deleted in D-b2.
3. **D-b1 lands only after the R2 deploy,** so R2 ships a binary without D-b1 behaviour, even with the switch off.
4. Owner questions 2 and 3 follow from owner decision 2 of 2026-10-06 ("One world means one Coinbase connection: D-b is required, not optional"): defaults applied (a capture outage blanks the live book and trades, shown as invalidated; only captured products can be opened). The owner can still overrule before D-b1 deploys. Question 1 (stop the `data/market` tick logs) is D-b2 only and is asked then.

5. (2026-10-07, from the owner audit; see docs/research/2026-10-07-audit-triage.md item 10) Correction to finding 11: `sentinel-backtest` reads the TickBinaryLogger trade files, so D-b2 must keep the logger or give sentinel-backtest the journal trades as its input. Also: steady state is 7 fan-out clients (one per product), not 1; the runbook preconditions use 7.

6. (2026-10-07) Correction: TickBinaryLogger writes trades only (no logBookUpdate call). Readers: sentinel-backtest via MarketEventSource.cpp TickBinaryTradeEventSource, and the Paper Trading dock which launches it. D-b2 must keep trade logging on the journal-fed path or add journal trades as an IMarketEventSource. See docs/research/2026-10-07-s8b-config-map.md.
