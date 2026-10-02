# One Coinbase WebSocket connection per product (server + capture)

Status: plan, 2026-10-02. Owner decision approved ("send it"). Builds on
`lt-astra/unsub-scope` (worktree `/Volumes/T7/sentinel-worktrees/lt-astra-unsub-scope`,
commit 187b090), which lands first. Engine line numbers below refer to that branch;
everything else refers to `main` at 84d7557.

## 1. Why (what the shared connection costs today)

One `MarketDataCoreEngine` owns one transport, one `sequence_num` counter, one
heartbeat watchdog, one subscription list and one backoff:

- A sequence gap invalidates every product and reconnects the whole socket
  (`MarketDataCoreEngine.cpp:333-343`, `emitBookInvalidated("")`).
- Transport down invalidates every product (`emitConnectionStatus`, `:159-165`).
- A recorder resnapshot for one symbol reconnects and invalidates all
  (`requestResnapshot`, `:685-702`, with a global cooldown "whichever product asked").
- The subscription list is shared, which is how FM-139 happened (closing ETH-USD
  unsubscribed pinned BTC-USD for 5h34m).
- Capture service log 2026-10-01 (`sentinel-capture-20261001-112425-2727.log`):
  9 upstream disconnects in 13 h, every one took all 7 products down; at 19:05:51
  the new socket delivered no frame for 5 s, the 20 s watchdog fired, and all 7
  products were invalid for 27 s. Each reconnect refetches 7 snapshots.

Coinbase's own guidance (fetched 2026-10-02):

- "When subscribing to multiple channels or products, it is recommended to spread
  the load across different WebSocket connections." and "instead of subscribing to
  multiple high-volume products (like BTC-USD and ETH-USD) on the same connection,
  open separate WebSocket connections for each."
  [guides/websocket](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/guides/websocket)
- "After establishing a WebSocket connection, the server expects a subscription
  message to be sent within 5 seconds; otherwise, the connection will be terminated." (same page)
- "WebSocket connections and unauthenticated messages are each limited to 8 per
  second per IP."
  [websocket-rate-limits](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-rate-limits)
- "Sequence numbers are increasing integer values for each product"
  [websocket-overview](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview).
  We measured them as per-connection and contiguous across channels
  (`MarketDataCoreEngine.hpp:180-181`). With one product per connection the two
  readings coincide, which removes that ambiguity.

Unconfirmed (not on any docs.cdp page found): a maximum number of concurrent
connections per IP; a maximum number of products per connection; whether the 8/s
limit counts failed handshakes. 8/s is a rate, not a concurrency cap. Treat the
concurrent cap as unknown and verify it live (section 9).

## 2. Target architecture

Decision: **one single-product engine per connection, N engines share one
`io_context` and one I/O thread**, grouped by a small owner object.

- `MarketDataCoreEngine` becomes a single-product, single-connection object:
  constructor `(Authenticator&, const ServerMdcConfig&, std::string product,
  net::io_context&, ssl::context&, TransportFactory, ReconnectPolicy)`. It no
  longer owns `m_ioc`, `m_ioThread`, `m_sslCtx` or `m_workGuard`
  (`MarketDataCoreEngine.hpp:158-173`); it keeps its own strand, both timers, the
  transport, its backoff, `m_lastSequenceNum`, one `ProductLiveness`. Delete
  `m_products`, `m_subscriptions.setDesiredProducts`, the liveness map,
  `subscribeToSymbols`/`unsubscribeFromSymbols` (`:175-209`), and the dead
  `m_lastSeqByProduct` + `m_seqMutex` (`hpp:177-178`, only ever cleared at `:100-103`).
- New `libs/core/marketdata/MarketDataFeeds` (name open) owns the `io_context`,
  the work guard, the one `mdc-io` thread (`run()` loop moves here, `:246-267`),
  the `ssl::context` (CA bundle loaded once, `:53-63`), a connect-rate limiter
  (section 5) and `std::unordered_map<product, unique_ptr<Engine>>`. API:
  `add(product)`, `remove(product)`, `requestResnapshot(product)`, plus the same
  callback setters as today. Every callback already carries `productId`;
  `onConnectionStatus` changes to `(const std::string& product, bool)`.
  Engines are created/destroyed on the main thread; `remove` posts `stop` to the
  strand, waits for the transport's down, then destroys. No engine ever sends an
  unsubscribe frame: the connection is closed instead.
- Why one thread, not thread-per-connection: the combined feed is ~50-90 frames/s
  (capture stats: 4.07 M frames in 13 h over 7 products); one thread parsing JSON
  is far below one core. Thread-per-connection adds 8-10 mostly idle threads on
  the owner's 16 GB Mac, and gains nothing because the only blocking work in a
  callback is the capture's non-blocking `Session::submit` and the server's
  `QueuedConnection` hand-off (`SentinelServerApp.cpp:111-147`). Keep one strand per
  engine so the thread count can grow later without touching engine code. Replace
  the `thread_local levelUpdates` (`:574-578`) with an engine member; the comment
  there already asks for this.
- Dispatch is unchanged: `MessageDispatcher::parse` and the L2/trade handlers stay
  per engine. `ServerDataModel` already keys everything by product
  (`ServerDataModel.hpp:79-87`); only `onMarketDataConnectionChanged(bool)`
  (`.cpp:234-236`) and `RecorderStallMonitor::setConnected` (`.hpp:29-33`, one bool)
  become per symbol. The stall monitor gates each series by its own symbol's
  connection state.
- The GUI's `MarketDataCoreQt` wraps the engine directly (`libs/gui/marketdata/MarketDataCoreQt.hpp:37`)
  but nothing registers it (`ServiceLocator::registerMarketDataCore` has no caller).
  It is dead; delete it in the cleanup slice rather than port it (open question 5).

## 3. Server lifecycle

`SentinelServerApp` replaces its one engine (`SentinelServerApp.cpp:85-91`) with
one `MarketDataFeeds`.

- Pinned recorder symbols (`default_symbols`, `config/server_config.yaml:43`,
  `m_defaultSymbols` `SentinelServerApp.hpp:29`): `add()` at init (`:210-231`),
  never removed. No code path can touch them: GUI symbols have their own engines.
- GUI symbols: `clientSubscribed` -> `add(symbol)` if not present (`:183-190`);
  `clientUnsubscribed` -> `remove(symbol)` unless pinned (`:192-205`). The pinned
  check stays as a guard, but the FM-139 class of bug is gone by construction:
  there is no shared desired set to rebuild.
- `recordingResnapshotRequested` (`:155-159`) -> `requestResnapshot(symbol)`,
  which reconnects only that symbol's engine. The 20 s cooldown becomes per engine
  (`ReconnectPolicy::resnapshotCooldown`, `hpp:69-71`); delete the "one stuck
  consumer must not keep gapping the rest" rationale, it no longer applies.
- Latency broadcast (`:165-181`) is fed by every engine; keep as is (it is a
  Coinbase-side latency, not per product).
- Optional cap `server.mdc.max_connections` (default 8, open question 4): `add()`
  beyond the cap logs an error and refuses; the stream server's client gets no
  data for that symbol. Without a cap, a GUI with many charts can open many
  connections against an undocumented per-IP limit.

## 4. Capture

`sentinel-capture` today: one engine for 7 products (`CaptureApp.cpp:136-174`), one
`Session` with 7 writers, v2 routing receipts (`CaptureSession.cpp:23-31`, `:155-176`),
one `connection` counter (`CaptureApp.cpp:128`).

Target: one engine **and one single-product `Session` per product**, in one process.

- Each product gets its own `connection` counter, `disconnectedSince`, 60 s
  supervisor (`:189-199`) and ingest observer; `IngestObservation.product` is set
  on every record. Header gains nothing new.
- Format: **no version bump.** RAWL2 v1 already means "one product, one connection":
  `format_version` is 1 whenever the header has no `connection_products`
  (`RawCapture.cpp:247-248`), and `docs/RAW_CAPTURE.md:33` documents the single-product
  v1 layout. Per-product connections make every stream a v1 single-product run with
  its own `run_id`. The verifier handles concurrent independent runs already
  ("each contribute their own span", `RAW_CAPTURE.md:196-197`) and keys newest runs
  per symbol (`CaptureVerifier.cpp:1000-1010`).
- Delete the v2 **writer** path: `Session` multi-product constructor and routing
  batch (`CaptureSession.cpp:23-31`, `:129-176`, `:235`), `RoutingBatch`,
  `frameReceipt` as a writer dependency, the 7-product routing benchmark and the
  "connection-wide gap marker in every product" semantics. Keep `RoutingId`,
  `frameIdentityLine`, golden vectors and the v2 **reader/verifier** path
  (`CaptureVerifier.cpp:856-862` multi detection) so the v2 archive written
  2026-09-30..10-02 on T7 stays verifiable (open question 3).
- Verifier changes: none required for correctness. `empty_connections` already
  counts per product/connection (`CaptureVerifier.cpp:378-392`) and now equals real
  sockets; the trade-id audit's "originating connection" is already the record's
  connection id and now identifies that product's own socket, so
  `reconnect_trade_gaps` reflect only that product's reconnects. Update
  `RAW_CAPTURE.md:111-122` (the "one silent connection counts as seven" sentence
  becomes false) and the launchd section.
- Queue: `--queue-mib` stays the process total; each session gets `total / N`.
  Any session failure stops the whole process (keeps the exit-nonzero/launchd
  restart contract, `CaptureApp.cpp:184-187`, `:216-218`).
- Stats line (`:200-207`) prints per product: `BTC-USD conn=3 frames=.. queued=..`.
- Tests: `capture_app_fixture.cpp:23-58` builds one engine; change `makeEngine` to
  take the product and build one scenario per product; `test_capture_app.cpp:126,130`
  (`FIXTURE_ENGINE` count 1, 6 sends on one transport) become N engines and 3 sends
  per connection lifetime. Delete the multi-product routing regressions that test
  the writer; keep the verifier-side ones over fixture v2 files.

## 5. Failure isolation semantics (per engine)

| Event on product P | Today | Target |
|---|---|---|
| sequence gap | invalidate all, reconnect socket | invalidate P, reconnect P |
| transport down / connect fail | invalidate all | invalidate P, backoff P |
| 20 s no inbound frame | reconnect socket | reconnect P |
| 30 s no L2 while heartbeats flow | unsub/sub L2 for P, escalate to reconnect if no healthy peer (`:648-683`) | invalidate P, reconnect P with per-engine doubling retry (30 s -> 10 min), reset on first update after a snapshot |
| malformed L2 | invalidate P, reconnect socket | same, P only |
| recorder resnapshot(P) | invalidate all, reconnect socket, global cooldown | invalidate P, reconnect P, per-engine cooldown |
| GUI closes P | unsubscribe frames on shared socket | close P's socket; nothing else changes |

Reconnect stagger. Capture (7) and the server (1 + GUI symbols) share one public IP,
and Coinbase allows 8 connections/s/IP. After a network blip every engine's backoff
timer fires in the same second. Two layers, both in `MarketDataFeeds`:

1. Per engine: `delay = backoff + jitter(0..1000 ms)`, jitter re-drawn each attempt.
2. Per process: a token bucket of 3 connects/s (so capture + server together stay
   under 6/s); `transport->connect()` is queued behind it. The 5 s subscribe
   deadline is unaffected (subscribe is sent on transport-up, `:105-107`).

## 6. What lt-astra/unsub-scope logic to delete or simplify

| unsub-scope piece | Fate |
|---|---|
| Delta subscribe/unsubscribe frames, `level2Only`, heartbeat-keep rule (`SubscriptionManager.hpp:18-26`, `:35-37`; INV-092) | Delete. `SubscriptionManager` shrinks to "three subscribe frames for one product"; no unsubscribe frame is ever built. INV-092 is superseded: the invariant becomes "a product's feed is its own connection". |
| `ProductLiveness` map, `healthyPeer`, `reconnectEscalations`, quiet-snapshot hashing (`snapshotHash*`, `comparableSnapshot`, `quietMs`, `mixSnapshotBits`, `:485-492`, `:544-557`) | Delete. One liveness struct with `lastLevel2Ms`, `snapshotAccepted`, `retryMs`. |
| L2 silence -> unsubscribe/subscribe pair (`:677-681`) | Delete; action is reconnect this engine (open question 2). |
| Ack reconciliation loop over `m_products` (`:362-372`) | Simplify: the ack on this connection must list this product under level2, else one warning. Keep `MessageDispatcher` ack parsing. |
| `requestResnapshot` global cooldown + "every book" invalidation (`:685-702`) | Per engine, invalidates only its product. |
| Capture replay honoring product-scoped markers (`CaptureVerifier.cpp:556-575` diff) | Keep: the verifier still reads the v2 archive. New captures only ever carry their own product. |
| `test_market_data_engine_reconnect.cpp` multi-product unsub tests | Rewrite as single product; the isolation tests in section 8 replace them. |
| `MessageDispatcher::parse` ack `level2ProductIds` | Keep. |

## 7. Resource cost

- Sockets: capture 7, server 1 pinned + GUI symbols (typically 1-3): ~10 TLS
  sockets, one `Connection` each (`BeastWsTransport.hpp:67-73`: ws stream, flat
  buffer, write queue). Estimate well under 1 MB each including OpenSSL buffers;
  total under 10 MB. Negligible against the recorder.
- Threads: +0 (shared thread) in each process. Timers: 2 per engine plus the
  transport's 3, all on the shared `io_context`.
- Snapshots on reconnect: one product's snapshot instead of seven (BTC's is the
  largest; this is the biggest bandwidth win).
- JWT: `use_jwt` is false in production (`server_config.yaml`, public channels). When
  on, `createJwt()` runs once per subscribe frame per connection (`:291-301`, ES256
  sign, sub-millisecond); no change in kind.
- Coinbase side: 8-10 concurrent connections from one IP. Unknown cap (section 1).
- Memory per engine: JSON parse buffers are per frame, unchanged; `levelUpdates`
  becomes one vector per engine.

## 8. Observability and tests

Logs: every engine line carries `product=<id> conn=<n>` (connection ordinal per
engine; the capture's record connection id is the same number). Thread stays
`mdc-io`. `MarketDataFeeds` prints one line per 60 s:
`Feeds: engines=8 up=8 | BTC-USD up conn=3 seq=140975 l2AgeMs=120 hbAgeMs=800 reconnects=2 ...`.
Probes: `ws.rx` gains the product prefix; add `feeds.connect` (token-bucket waits).

Tests (all offline, `FakeWsTransport` per engine; `TransportFactory` receives the
product so a test maps product -> `WsScenario`):

- Gap on ETH leaves BTC's book valid and BTC's transport untouched (no close, no
  invalidation callback for BTC).
- 20 s silence on ETH reconnects only ETH; BTC frames keep flowing throughout.
- 30 s L2 silence on ETH with heartbeats: ETH invalidated + reconnected with doubling
  retry; BTC untouched.
- `remove(ETH)` closes ETH's transport; BTC never receives a frame send, no
  `onConnectionStatus` for BTC. (FM-139 regression.)
- `requestResnapshot(ETH)` reconnects ETH only; a second request within cooldown is
  ignored for ETH but accepted for BTC.
- Stagger: 8 engines downed at once connect no faster than the token bucket, each with
  jitter; none later than backoff + 1 s + bucket wait.
- Server: `ServerDataModel` stall monitor warns for ETH only while BTC is connected.
- Capture: 3 products, ETH reconnects twice: ETH's file shows connections 1..3, BTC's
  shows 1; `--verify` root reports `connections` per product, `empty_connections` 0,
  v1 headers, no receipts.
- Verifier: existing v2 fixtures still pass (reader kept).

## 9. Rollout (one agent per slice; land one at a time)

0. Land `lt-astra/unsub-scope` (in review).
1. **Core** (`lt-*/feeds-core`): single-product engine + `MarketDataFeeds` + stagger
   + tests above. Server and capture compile against the new API with their current
   behaviour (server adds pinned + GUI symbols, capture adds 7). Docs: MARKETDATA.md.
   Hot-file note: touches `MarketDataCoreEngine.cpp` only; do not pair with S2/S3.
2. **Capture** (`lt-*/feeds-capture`): per-product sessions, delete v2 writer, update
   RAW_CAPTURE.md and the launchd section (arguments unchanged). Deploy with
   `scripts/dev/deploy-runtime.sh capture` (60 s verify looks for `Capture stats`,
   auto-rollback). Soak: after 1 h and 24 h, `sentinel-capture --verify /Volumes/T7/sentinel-data/raw-l2`
   on the new run: per-product `connections`, `sequence_gaps` 0, `empty_connections`,
   `reconnect_trade_gaps` per product; run log `rg 'no inbound|Heartbeat stale|rate|429|1008'`
   for any sign of a per-IP connection cap.
3. **Server** (`lt-*/feeds-server`): `SentinelServerApp` + `ServerDataModel`/stall
   monitor per symbol + `max_connections`. Deploy `deploy-runtime.sh server` (verify
   looks for `Recording v2 started`). Soak 24 h: `hmc2_dump <recording.dir> BTC-USD near 60000 120`
   shows continuous columns across a GUI open/close of ETH-USD (the FM-139 repro),
   `rg ' [WEF] ' sentinel-server-latest.log` shows per-product invalidations only.
4. **Cleanup**: delete `MarketDataCoreQt` (if agreed), `_agent` updates (INV-092
   superseded, new INV for "one product per connection", FM-139 guardrail text),
   CONFIG.md for `max_connections`, DECISIONS entry.

Capture before server: the product set is static, the verifier is an independent
judge, and the 24 h capture soak answers the per-IP connection question before the
recorder depends on it. Reviews: the other vendor reviews each slice; slice 3 also
gets the extra Fable review (always-on recorder rule).

## 10. Risks

- Undocumented per-IP concurrent connection cap: 8-10 sockets may be fine or may be
  refused/closed. Mitigation: capture soak first; the stats line shows `up=` per
  engine; keep the server's `max_connections`.
- 8 connections/s/IP on a reconnect storm: covered by jitter + token bucket; both
  processes are independent so the combined rate is bounded by 2 x 3/s.
- A quiet product (low-volume) with 30 s L2 silence now reconnects instead of
  resubscribing: with the doubling retry (to 10 min) this is at most a few reconnects
  per hour and each shows as a reconnect_trade_gap in the verifier. If live data
  shows this on PEPE/FARTCOIN, raise `level2Stale` per product or restore the
  resubscribe-first step (open question 2).
- Capture queue split (`total / N`): BTC gets 1/7 of 64 MiB; a disk stall now fails
  sooner for BTC. Mitigation: weight by observed frame share or raise `--queue-mib`
  in the plist (owner deploy).
- Deleting the v2 writer leaves the verifier's v2 path exercised only by fixtures.

## 11. Open questions for the owner

1. Thread model: one shared I/O thread for all engines (recommended) or one thread
   per engine?
2. L2 silence action: reconnect only (recommended, deletes the resubscribe dance and
   the snapshot fingerprinting) or keep an L2 resubscribe as the first step?
3. RAWL2 v2: delete the writer and routing receipts but keep the verifier's v2 reader
   for the 2026-09-30..10-02 archive (recommended), or delete both and verify that
   archive only with the old binary?
4. Server cap on GUI-driven connections (`server.mdc.max_connections`, default 8) and
   the behaviour when exceeded (refuse vs evict the oldest GUI symbol)?
5. Delete the unreferenced GUI `MarketDataCoreQt` in the cleanup slice?
6. Rollout order: capture first (recommended) or server first because FM-139 is the
   active pain?
7. Per-product queue split in capture: equal `total / N` or weighted (BTC heavier)?

## 12. Owner decisions (2026-10-02, approved)

1. One shared I/O thread for all engines, one strand per engine.
2. L2 silence on a per-product connection: reconnect that product only (no resubscribe step, no snapshot fingerprinting).
3. Delete the RAWL2 v2 writer and routing receipts; keep the verifier's v2 reader for the 2026-09-30..10-02 archive.
4. `server.mdc.max_connections` default 8; beyond the cap REFUSE (never evict a watched chart). The refusal is visible to the developer: `sLog_Error` with symbol and cap, the requesting client gets an explicit error the chart shows, and the Agent API / server status reports the refused symbol and the cap. Pinned recorder symbols never count against the cap.
5. Delete the unreferenced GUI `MarketDataCoreQt` in the cleanup slice.
6. Rollout: capture first, then server (FM-139 is already fixed by lt-astra/unsub-scope).
7. Capture queue: ONE shared pool across products (no static split) with a configurable per-product floor, default 2 MiB, the rest globally available. Owner allows more memory: total default raised from 64 MiB to 512 MiB (measured 2026-10-01: ~98 KB/s average raw frame rate for 7 products, so 64 MiB covered ~11 min of disk stall vs the 39-min TCC freeze, FM-127; 512 MiB covers ~90 min). The queue must allocate only as frames are queued (steady state is ~0 bytes queued), never reserve the pool up front. Both values configurable (`--queue-mib`, `--queue-floor-mib`).
