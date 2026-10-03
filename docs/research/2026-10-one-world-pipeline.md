# One world: journal -> roller -> chunks (plan)

Status: decision plan, 2026-10-02. Owner direction (approved): one stream per asset
(book + trades) streams and stores raw; a roller turns the raw into heatmap chunks;
delete the redundancy that keeps two worlds. Builds on
`docs/research/2026-10-per-symbol-connections.md` (per-product connections, RAWL2 v1 per
product, shared queue pool; slices 1-3 in flight). Line numbers refer to `main` at 30f17d4.
Original read-only study; see "Slice A as built" below for implementation and the measured parity difference and review acceptance bands.

Implementation status and refinements are recorded in the "as built" sections below.

## 0. Today, measured (two worlds)

| | sentinel-capture (`com.sentinel.capture`) | sentinel-server recorder (`com.sentinel.recorder`) |
|---|---|---|
| Feed | own Coinbase socket, 7 products (`CaptureApp.cpp:136-177`) | own Coinbase socket, pinned BTC-USD + GUI symbols (`SentinelServerApp.cpp:67, 167-215`) |
| Output | RAWL2 hour files, `/Volumes/T7/sentinel-data/raw-l2/<product>/YYYY/MM/DD/HH.rawl2` | HMC2 day files, `/Volumes/T7/sentinel-data/recording/<symbol>/<layer>-<tf>/YYYY-MM-DD.hmc2` |
| Archive on disk | 2.2 GB; BTC from 2026-09-30 00:00 UTC (v1 run `61fdb803`), the other six from 2026-10-01 04 UTC (v2 run `49ffaa57`, 7-product routing) | 84 MB; BTC from 2026-09-28 (generations `.g1/.g2` from the $10 -> $5 deep change), ETH-USD as a side effect of a GUI subscription (0.2-0.4 MB/day) |
| Rate, BTC | hour 2026-10-01 10 UTC (`--verify`): 74,727 frames = 64,235 l2_data + 3,600 heartbeats + 6,892 trade frames (12,672 trades); 20.8 frames/s mean, p99 27/s; 122.8 MB JSON/h (2.95 GB/day); 12.5 MB/h on disk (300 MB/day, zstd 9.8x) | near 1m 7.2-9.7 MB/day (4.3k entries/column), deep 1m 7.1-8.1 MB/day (18.3k entries/column), deep 1h 1.3-1.5 MB/day; about 17 MB/day |
| Rate, 7 products | capture stats 2026-10-02 01:29-16:54: 5.03 M frames (91/s), 6.78 GB JSON (10.5 GB/day), 799 MB on disk (1.24 GB/day) | n/a |
| Live | none | `publishOpen` every 500 ms of integration clock (`BookRecorder.cpp:370-423`), LiveService 100 ms turns, cadence 250 ms (`RecordingLive.hpp:82-94`, `.cpp:297`); GUI data age about 0.4 s |

Both processes parse the same Coinbase JSON on their own socket, so the two stores are
not derived from each other: the recorder's BTC HMC2 and the capture's BTC RAWL2 saw
different connections (different snapshot times, reconnects, 27 s outage on 2026-10-01
19:05 in the capture only). This matters for the parity test (section 7).

## 1. Architecture and process boundaries (decision)

```
Coinbase ──(1 ws per product)──> sentinel-capture ──> journal (RAWL2 v1 hour files, fsync per block)
                                      │ fan-out (Unix socket, in-memory ring)
                                      v
                        sentinel-server: JournalFeed ──> roller (BookRecorder) ──> HMC2 ──> ChunkService / LiveService ──> GUIs
                                                    └──> live book, trades (SymbolHotData, TPO/footprint, candles)
sentinel-roll (batch CLI): JournalFeed over files ──> same roller library ──> HMC2 (backfill, repair, parity)
```

Decisions:

1. **The roller is a library (`libs/core/roller`), hosted by two programs**: the live path
   inside `sentinel-server`, and the batch converter `sentinel-roll`. Both drive the same
   `BookRecorder` with the same event stream and the same clock rule (section 4), so
   `live == replay` is a property of one code path, not of two implementations kept in
   sync. A separate `sentinel-roller` launchd service was rejected: it would need a new
   cross-process protocol for the 2 Hz open-minute publications (`Hmc2Record`, 4-18k
   entries, two layers) and for the chunk watermarks (`ChunkService::effectiveWatermarks`
   reads `BookRecorder::watermarks` in-process, `ChunkService.cpp:35-46`,
   `SentinelStreamServer.cpp:1859-1868`), plus a third always-on binary under TCC (FM-127).
   The library boundary keeps that option open if the server ever needs to restart often.
2. **Two always-on services remain**: `sentinel-capture` (ingest + journal + fan-out; the
   only process that talks to Coinbase) and `sentinel-server` (roller + serving). The
   server stops being data-critical: its state is rebuilt from the journal after any
   restart (section 4), so a server redeploy never loses data.
3. **Live fan-out = Unix domain stream socket** (`~/Sentinel-runtime/run/capture.sock`),
   one framed record per journal record, with a per-product in-memory ring of the last
   60 s (section 3). Shared memory was rejected (no benefit at 20-90 records/s, harder
   crash semantics); in-process (roller inside capture) was rejected because ingest must
   stay dumb and stable, and the always-on capture must not carry rollup logic that
   changes weekly.
4. **The server owns the live order book GUIs see**, exactly as today
   (`SymbolHotData.liveBook`, `bookUpdateBroadcast`, `ServerDataModel.cpp:452-460, 490-520`),
   fed by `JournalFeed` instead of `MarketDataCoreEngine`. GUIs stay remote-only (INV-006);
   nothing in the GUI changes.
5. **Trades follow the same path**: `ServerDataModel::onTrade` (`.cpp:404-430`), the
   footprint/TPO tape, `TradeOverlayPublisher` and `TimeframeAggregator` are fed from the
   journal's `market_trades` frames by `JournalFeed`. REST candle history (F10) stays REST.
6. **The product set is the capture's set** (7 products, launchd arguments). After
   cutover the server can serve only journaled products; a GUI request for another symbol
   is refused with the same visible error path as `max_connections` (per-symbol decision 4).
   Dynamic product add through the control channel is later work (owner decision 2).

## 2. Journal semantics (read-while-write)

RAWL2 v1 already is the append-only journal the owner asked for; no format bump.

- **Framing** (`docs/RAW_CAPTURE.md:508-557`): header, then blocks `BLK1 | clen | rlen |
  records | first/last systemNs | run-wide block ordinal | raw CRC | header CRC | zstd
  frame`, closing index `IDX1`. Each decoded record: `len | kind | systemNs | steadyNs |
  connection | payload` (`RawCapture.cpp:323-328`). Kind 1 is the exact pre-parse
  WebSocket text (`MarketDataCoreEngine.cpp:116`, observed before `json::parse`).
- **Flush/fsync**: a block closes at 1 s or 1 MiB (`RawCapture.hpp:47-49`,
  `RawCapture.cpp:315-317, 333-335`) and is `F_FULLFSYNC`ed (`fsync_blocks=1`,
  `RawCapture.cpp:353`, `PersistenceIo.hpp:22-31`). At most one block (1 s) plus the queue
  is in RAM at a crash.
- **Sealing**: an hour or index-full segment ends with `IDX1` (`RawCapture.cpp:357-366`);
  files are exclusive-create, never reopened for append (`:281-287`), so a sealed file is
  immutable by construction and a sealed hour is "the file has `IDX1`".
- **Position**: `JournalPos = (product, run_id, block ordinal, record index)`. All three
  exist on disk today (file header `run_id`, `first_block_ordinal`; block header
  `ordinal`); no field is added. A reader maps an ordinal to a file through the headers.
- **Tailing an open file**: the reader snapshots the file length at open and treats an
  incomplete trailing block as a torn tail (`RawCapture.cpp:406-470`). The tailing rule:
  bytes after the last complete block are *pending*, not torn, until one of (a) the file
  has `IDX1`, (b) a later segment of the same run exists (`segment` in the header), or
  (c) a newer run supersedes it (`run_started_system_ns, run_id` order, `RAW_CAPTURE.md:95-97`).
  Only then are pending bytes torn (lost; the verifier already reports them). One reader
  change: `hasFollowingFraming` (`RawCapture.cpp:436`) must not declare interior corruption
  on a tail that is still pending; defer the check until the file is sealed/superseded.
- **Gaps are explicit**: transport down/up, invalidations, resync requests, and a
  stop-with-gap record (`RawCapture.hpp:21-24`, `RAW_CAPTURE.md:534-542`). The roller
  maps them to `onInvalid`; a crashed capture leaves a torn tail, which the roller treats
  as invalid from the last complete record until the next snapshot (INV-055).

## 3. Live fan-out

- Capture's ingest observer (`CaptureApp.cpp:141-169`, called on the `mdc-io` thread
  before parse) gains a second sink: a per-product ring buffer (60 s or 32 MiB, whichever
  first) and the socket server. Records are written to subscribers from a dedicated
  fan-out thread with a bounded per-subscriber send buffer (16 MiB); a subscriber that
  falls behind is disconnected and resynchronises (next item). The disk path is untouched.
- Wire: `hello{product, resumePos}` from the subscriber; capture answers `tip{pos}` and
  streams from `resumePos` if it is inside the ring, else from the ring's oldest record
  and the subscriber first reads files up to that position. Each wire record = JournalPos
  + the on-disk record bytes (same 32-byte header + payload). Control messages the other
  way: `resnapshot{product}` (replaces `BookRecorder::onSelfInvalidated` ->
  `MarketDataCoreEngine::requestResnapshot`, `ServerDataModel.cpp:190-196`,
  `SentinelServerApp.cpp:138-142`): capture reconnects that product's engine and the
  snapshot lands in the journal, so replay sees the same thing live saw.
- Live = replay caught up to the tip: the roller always starts from its checkpoint
  (section 4), reads files, switches to the ring/socket, and from then on the socket is
  the only source. If the socket drops (capture restart, FM-127 freeze), the roller
  tails files and reconnects; the gap it sees is the journal's own, nothing else.
- Latency: one local socket hop (sub-millisecond) replaces the Qt queued hop from
  `mdc-io` to the main thread (`SentinelServerApp.cpp:94-130`); JSON parse moves from the
  server's `mdc-io` thread to the server's `JournalFeed` thread, same cost once per
  process. See section 6.

## 4. Roller: determinism, checkpoints, exactly-once, repair

### 4.1 What in BookRecorder depends on wall clock or arrival timing (must change)

| Today | Why it breaks `replay == live` | Change |
|---|---|---|
| `localClock()` sampled at enqueue (`BookRecorder.cpp:867-877`), default `systemNow()` (`:88-91`) | the host clock at enqueue time is not in the journal | public API takes `localMs` per call; the driver passes the record's `systemNs / 1e6` (journal receive time). The injected-clock constructor goes away. |
| `onTick(localNowMs())` from a 250 ms QTimer (`ServerDataModel.cpp:209-212`), applied at `t = time - offset` (`BookRecorder.cpp:812, 849`) | tick moments are arrival timing; they decide when minutes close (`advance`, `:463-487`) and commit (`commit`, `:621, 648-649`), hence `kLateEvents` and the lateness cutoff | the driver synthesises one tick per journal record of any kind (heartbeats are 1 Hz, `channels.heartbeats` 3,600/h), with `local = record systemNs`. Identical stream in batch and live. Idle advance granularity becomes 1 s instead of 250 ms; minute content is unchanged because minutes close at exact boundaries regardless of tick timing. |
| Queue overflow -> `InvalidEnvelope`/emergency invalidation (`:195-218, 229-240, 826-832, 854-858`), `kQueueSlots=4096`, `maxQueuedLevels` | a batch producer outruns the worker and fabricates invalid intervals that live never had | blocking enqueue (bounded queue, producer waits). In live mode the backpressure reaches the socket; capture drops a slow subscriber and the roller resumes from the ring/files. Data is never turned into a gap by load. Delete `queueDrops`. |
| `onSelfInvalidated` -> engine reconnect (`:272-286`, backoff on `workerLocal`) | the roller has no socket to reconnect | control message to capture (section 3); backoff timing uses journal time (already `m.local`), so batch and live agree; in batch the request is a no-op (the journal already contains whatever snapshot came). |
| `exchange_timestamp = now()` when `timestamp` is absent (`MarketDataCoreEngine.cpp:469`); trade `time` fallback `now()` (`MessageDispatcher.hpp:43-45`); snapshot `exchangeNowMs()` fallback (`ServerDataModel.cpp:493`) | host clock leaks into integration time | `JournalFeed` substitutes the record's receive time. Never seen on Coinbase data so far (`late=0`, `backward=1222` in 5 h are envelope-driven), but the rule must be fixed. |
| `restoreHours`/`rebuildHour` read HMC2 at the first snapshot (`:495-522`) | state depends on disk contents | keep: both hosts start from the same checkpoint and the same files; the hour rollup is a pure function of committed minutes. |
| Throttled error log on `workerLocal` (`:411-419`) | logging only | keep (journal time). |

Everything else is already deterministic: integration clock = envelope time made
monotone (`:848`), `kResynced` on snapshot (`:713`), peaks after atomic batch apply
(`:781-783`), windows from `midMin/midMax` (`:394, 436`), schema-3 deltas from the
previous record (`Hmc2Store.cpp:1264-1266`), hour rollup from minutes.

Parsing must be bit-identical to the recorder's input: `JournalFeed` reuses
`MarketDataCoreEngine::handleOrderBookSnapshot/Update` and `parseLevel`
(`MarketDataCoreEngine.cpp:528-630`, including the zero-size snapshot filter at `:543-544`)
and `MessageDispatcher::parse` for trades. These move out of the engine into
`marketdata/dispatch` so the engine (capture) and the feed (server, converter) share
them. The verifier's `DecimalGrid` integer path (`CaptureVerifier.hpp:9`) is not used
for rollups: the owner's native-precision decision (storage pyramid, 2026-09-30) is
kept by the price grid, not by changing the parse path, and switching to integer
parsing would break parity with the existing HMC2.

Per-product grids are a prerequisite for rolling the other six products: the layer
config is global today (`ServerDataModel.cpp:171-174`, `priceScale=100`, `near_tick=1`,
`deep_tick=5`, `size_floor=1e-8`, `config/server_config.yaml:22-32`), and `priceUnits`
rejects PEPE prices at `priceScale=100`. The roller derives `priceScale` from the
journal header's `product_metadata.quote_increment` and ticks from the 1 bp rule
(DECISIONS 2026-09-27: 1-2-5 rounding, never below the quote increment, fixed per
recording day) unless `recording.products.<id>` overrides it (owner decision 3).

### 4.2 Checkpoints and exactly-once

- Checkpoint file `<hmc2 root>/<symbol>/roller.json`: `{pos: JournalPos of the last applied
  record, committedThroughMs, configHash, rollerVersion}`; written with tmp + rename +
  directory fsync (reuse `PersistenceIo`) after every `commit()` that wrote a column
  (at most once per minute). Write order: HMC2 append + fsync (`Hmc2Store.cpp:1283-1301`)
  first, checkpoint second.
- Restart: locate the newest snapshot frame at or before `pos` (connections start with
  `TransportUp` + snapshot; the verifier already does this walk), replay from there with
  `commitFloorMs = committedThroughMs`: minutes below the floor are rebuilt in memory
  (the book must converge) but not written; the open minute and the lateness tail
  (`pending`, dropped by the destructor today, `BookRecorder.cpp:193`) are re-derived
  exactly. Exactly-once is therefore idempotent rebuild + suppression; a crash between
  append and checkpoint re-appends one byte-identical record for the same bucket, which
  readers already resolve by last-record-per-bucket (INV-056, `Hmc2Store.cpp:1073`).
- Replay cost bound: one connection's worth of journal (reconnects run about every
  1.5 h on the capture log; a quiet day could mean 24 h = 2.95 GB of BTC JSON). Slice A
  measures parse+apply throughput; if a restart can exceed about a minute, add an hourly
  roller state checkpoint (book levels + validity + pending records, about 0.2 MB
  compressed per product) so replay never exceeds one hour. Decide on measurement, not now.
- Gap found later: nothing to do for the HMC2 already written, because a journal gap was
  already an invalid interval (`observedMs` excludes it, INV-057). A gap *filled* later
  (second node, section 4.3) is a repair.

### 4.3 Repair from a second journal

A second node (Pi/cloud) runs the same capture against its own Coinbase connection and
produces an independent journal. Repair = `sentinel-roll --repair --journal <B> --from
--to`: roll B's journal over the interval, and for each minute take B's record only where
A's is missing or has smaller `observedMs`. The repaired records are appended under the
same header (same config hash): `Hmc2Store::append` reuses the day file (`:1236-1248`)
and the later record wins. Two journals are never merged at the frame level: each has
its own sequence domain and snapshot timing; merging books is not needed because the
rollup is what gets repaired. Open: the HMC2 root lock (`Hmc2Store.cpp:1182`) stops the
batch tool while the server's roller holds the root; the repair runs during a planned
server restart, or the lock becomes per-symbol (open question 2).

## 5. Server after cutover

`SentinelServerApp` no longer builds a `MarketDataCoreEngine` (`:67-71`), the callback
bridge (`:94-146`), the subscribe/unsubscribe plumbing (`:167-215`), or
`broadcastCoinbaseLatency` from its own feed (`:148-165`; latency becomes a capture
metric relayed as a journal-derived value: record `systemNs` minus envelope time). It
builds one `JournalFeed` per product, which exposes exactly the engine's callback
surface (`onTrade`, `onLiveOrderBookLevelUpdates/Initialized/Invalidated`,
`onConnectionStatus(product, bool)` from `TransportUp/Down` records), so
`ServerDataModel` slots are unchanged. `RecorderStallMonitor::setConnected`
(`.hpp:30-34`) is driven per product by the journal's transport records plus a
"journal tip age" guard: a stalled capture is reported as the journal being stale, not
as a recorder stall. `ChunkService`, `LiveService`, `Hmc2Reader` and the wire are
untouched; the roller's `BookRecorder::watermarks` keep feeding `effectiveWatermarks`.

## 6. Latency budget

Today (`liveDataAgeMs`, `HeatmapTileNode.cpp:1136`; p50 about 0.4 s): Coinbase ->
`mdc-io` parse -> queued to main -> recorder queue -> worker; `publishOpen` at 500 ms of
integration clock; LiveService turn 100 ms, cadence 250 ms (FM-137); stream to GUI.

Target: Coinbase -> capture ingest copy (no parse) -> fan-out thread -> socket -> server
`JournalFeed` thread parse -> recorder queue -> worker; the rest identical. Added: one
socket hop and one thread hand-off, both well under 1 ms at 91 records/s, 122 KB/s total.
Removed: the main-thread queued hop. The 500 ms publication interval dominates, so the
age stays within measurement noise of today. Acceptance: GUI `liveDataAgeMs` p50/p95
over 10 min, before and after cutover, via the Agent API; regression if p95 rises by
more than 50 ms. `recording.live_publish_ms` and the cadence rule are unchanged.

## 7. Slice A: the converter and its parity test

`sentinel-roll <journal root> <hmc2 root> --products ... [--from --to]` = `JournalFeed`
over files + roller library + checkpoint. It reads v1 and the 2026-09-30..10-02 v2
archive (mixed-product envelopes are written unchanged into both products,
`RAW_CAPTURE.md:396-399`; the feed applies only events whose `product_id` is its own and
skips kind-9 receipts). Backfill: BTC from 2026-09-30 00:00 UTC, the six others from
2026-10-01 04 UTC; the recorder's 2026-09-28/29 BTC days exist only as HMC2 and are
copied into the new root as they are.

Parity test (`hmc2_diff <root A> <root B> <symbol> <layer> <from> <to>`): the two BTC
stores come from different connections (section 0), so whole-file byte parity is not a
valid test. The test that is valid:

1. **Determinism**: two converter runs over the same journal produce byte-identical HMC2
   files; the converter over files and the live roller over the socket (section 8, shadow)
   produce byte-identical records per bucket. This is the property the architecture needs.
2. **Recorder parity on qualifying minutes**: for every minute where both stores have
   `observedMs == 60000` and neither has `kResynced`, decoded entries (row, side, twapCode,
   peakCode), bounds and the four mid values are identical; flags compared with
   `kLateEvents` masked (lateness cutoffs depend on tick timing, section 4.1). Report the
   count of qualifying, matching, mismatching and non-qualifying minutes; a mismatch is a
   bug. Non-qualifying minutes are listed with the reason (the recorder's or the
   capture's own outage). Hour rollups: compared where every constituent minute qualifies.
3. **Throughput**: records/s and MB/s of JSON for parse and apply, reported per product;
   target: the whole archive (about 19 GB JSON) in under one hour on the owner's Mac,
   run at night, one builder at a time (machine budget).

Per-product grids (section 4.1) are part of this slice; without them only BTC rolls.

## 8. Migration and cutover without a recording gap

The journal is the truth from the moment the per-symbol capture slice lands; after that
any HMC2 gap is repairable by the converter, so "no recording gap" is guaranteed by the
journal, not by HMC2 continuity. Order:

1. Per-symbol slices (in flight: core, capture, server).
2. Slice A converter + parity; backfill into a **new root** `/Volumes/T7/sentinel-data/hmc2`
   (owner decision 4) while the recorder keeps writing `recording/`.
3. Slice B: capture fan-out socket, ring, control channel, `/metrics` on 8091 (the
   observability plan's slice 2). Deploy capture (`deploy-runtime.sh capture`).
4. Slice C: live roller in the server in **shadow mode**: the server keeps its own engine
   and recorder on `recording/`; a second `BookRecorder` fed by `JournalFeed` writes the
   new root; `hmc2_diff` runs hourly on both roots and the metric
   `sentinel_roller_shadow_mismatch_total{product,layer}` must stay 0 for 48 h. Deploy
   server (verify marker in `deploy-runtime.sh:36` changes to the new "Roller started" line).
5. Slice D cutover: `feed.source: journal` and `recording.dir: .../hmc2`; the engine,
   the old recorder instance and the engine bridge are compiled out; deploy server. The
   restart is a few seconds; the roller resumes from its checkpoint and catches up from
   the ring. Soak 48 h.
6. Rollback at any point: `feed.source: coinbase` + the old root; the old recorder resumes
   with a hole in `recording/` since cutover, which the converter can fill from the
   journal. Keep the binary of step 4 as `sentinel-server.rollback` (already the pattern).
7. Deletion slice (section 9) only after the soak.

Deploy rule stays FM-127: owner at the Mac, one service at a time, 60 s write check.
Capture deploys now also affect the live view (section 11), so bundle capture changes.

## 9. What gets deleted at the end

- Server: `MarketDataCoreEngine` ownership and bridge (`SentinelServerApp.cpp:65-215`),
  `requestResnapshot` wiring, `recordingResnapshotRequested`, `BookRecorder::localClock`,
  the queue-overflow invalidation path, `TickBinaryLogger` (legacy binary log,
  `ServerDataModel.hpp:117`), `sentinel_mdc_*` metrics on the server (they move to capture).
- Capture: already in the per-symbol plan (v2 writer, routing receipts); plus the
  supervisor's reasons move into the control channel log.
- Legacy heatmap phase 2 (`docs/research/2026-09-legacy-heatmap-removal-inventory.md:66-91`,
  `HeatmapTwapStreamer`, legacy wire): a separate existing checklist; it is the last
  piece of the second world and should land before or with the deletion slice.
- `tests/servermodel/storage_probe` (local-feed capture, declared not raw truth in
  DECISIONS 2026-09-29).
- Kept: RAWL2 format and verifier, HMC2 format, `Hmc2Store/Reader`, `BookRecorder` (as
  the roller core), `ChunkService`, `LiveService`, the wire.

## 10. Monitoring per stage (existing `/metrics`, `ops/monitoring/prometheus.yml:8-16`)

| Stage | Metric (new unless noted) | Alert |
|---|---|---|
| Ingest (capture :8091) | `sentinel_mdc_connected{product}`, `sentinel_mdc_last_l2_timestamp_seconds{product}`, `sentinel_capture_frames_total{product}`, `sentinel_mdc_ws_latency_ms{product}` (observability rows 7, 8, 12, 13 move here) | A2 per product |
| Journal | `sentinel_journal_flushed_ordinal{product}`, `sentinel_journal_tail_age_seconds{product}` = now minus last fsynced record, `sentinel_capture_queued_bytes` (row 15), `sentinel_capture_failure_markers_total` (row 18) | tail age > 5 s while connected |
| Fan-out | `sentinel_fanout_subscribers`, `sentinel_fanout_disconnects_total{reason}`, `sentinel_fanout_backlog_bytes{subscriber}` | any slow-subscriber disconnect |
| Roller (server :8090) | existing `sentinel_recorder_*` (`ServerDataModel.cpp:243-287`) kept; `sentinel_roller_lag_records{product}` = tip minus applied, `sentinel_roller_lag_seconds{product}` = now minus receive time of the last applied record, `sentinel_roller_source{product}` 1 = socket / 0 = files, `sentinel_roller_checkpoint_age_seconds{product}`, `sentinel_roller_shadow_mismatch_total{product,layer}` during shadow | lag > 5 s while journal tail is fresh; mismatch > 0 |
| Serving | existing `sentinel_recorder_last_column_timestamp_seconds` (A1), `sentinel_stream_sessions`; GUI `liveDataAgeMs` via Agent API (row 25) | A1 unchanged |

The recorder stall alert A1 keeps its meaning: last column age > 240 s while
`sentinel_mdc_connected == 1`, with the connected gauge now coming from the capture job.

## 11. Risks

- Capture becomes a single point of failure for the live view too (today a capture
  crash leaves GUIs alive). Mitigation: launchd KeepAlive, the roller tails files during
  a capture restart, and the second node is the real answer (step 5 of the direction).
- Every capture deploy is a TCC event (FM-127) and now also a live-view event: fewer,
  bundled capture deploys; owner present.
- Parity may fail for a legitimate reason (two connections): the test is defined on
  qualifying minutes (section 7); a mismatch there is a bug and blocks step 5.
- Backfill throughput with `nlohmann::json` is unmeasured; the budget (one hour for the
  archive) is a guess until slice A reports.
- Restart replay from the last Coinbase snapshot can be long on a quiet connection;
  section 4.2 has the fallback (hourly state checkpoint).
- Duplicate HMC2 records after crash loops are harmless to readers but grow files;
  `commitFloorMs` bounds them to one record per restart.
- `backward=1222` steps in 5 h on the live recorder are envelope-time regressions that
  the monotone clamp absorbs; the parity test will show whether both connections see the
  same regressions (they should, the timestamps are exchange-side).
- The 16 GB machine: backfill, shadow roller and the old recorder together; run the
  backfill alone at night and keep the shadow phase to one extra recorder instance.

## 12. Owner decisions (only these)

1. **Roller placement**: library hosted in `sentinel-server` plus the batch CLI
   (recommended), or a separate `sentinel-roller` launchd service (one more always-on
   binary, a new publication protocol).
2. **Product set after cutover**: fixed by the capture's launchd arguments; the server
   refuses other symbols (recommended now), with dynamic add through the control channel
   as later work.
3. **Per-product grids**: derive `priceScale` and ticks from the journal's product
   metadata by the 1 bp 1-2-5 rule (recommended, deterministic, no config to forget), or
   an explicit `recording.products` table. Either way `size_floor` per product comes from
   `base_increment`.
4. **New HMC2 root** `/Volumes/T7/sentinel-data/hmc2` for the one-world store, with the
   2026-09-28/29 BTC days copied in (recommended; keeps `recording/` as the rollback
   store untouched), or the converter writing new generations into `recording/`.
5. **Parity definition**: accept "run-to-run byte identity + decoded-record parity on
   qualifying minutes with `kLateEvents` masked" as the correctness test for slice A,
   replacing whole-file byte parity (not achievable across two connections).

## 13. Slices (one builder at a time)

| Slice | Content | Size | Who |
|---|---|---|---|
| S0 | per-symbol core / capture / server (in flight) | planned | as assigned |
| A | `libs/core/roller` (JournalReader v1/v2 with tailing rule, `JournalFeed`, driver with record-time ticks, blocking queue, `commitFloorMs`, checkpoint), parse code moved to `dispatch/`, per-product grids, `sentinel-roll`, `hmc2_diff`, backfill 7 products, throughput report | ~2k lines, 2-3 agent days | Codex astra; Claude review |
| B | capture fan-out: ring, socket server, wire, control `resnapshot`, `/metrics` :8091 rows 13/15-18, prometheus job | ~700 lines | Codex sol; Claude review; owner deploy |
| C | server shadow roller: socket client with file catch-up, second `BookRecorder` on the new root, shadow mismatch metric, hourly `hmc2_diff`, GUI age measurement | ~800 lines | Claude (visual check); Codex + Fable review (always-on) |
| D | cutover: `feed.source`, `JournalFeed` feeds `ServerDataModel`, engine compiled out, deploy, 48 h soak, rollback drill | ~400 lines net negative | Claude; Fable review |
| E | deletions (section 9), docs (`ARCHITECTURE.md`, `MARKETDATA.md`, `RAW_CAPTURE.md`), `_agent` entries | ~-1.5k lines | Codex sol |
| F | later: compression lab on the journal, second node + repair tool, Parquet/DuckDB research layer | open | |

## 14. Open questions (technical, not owner)

1. Hourly roller state checkpoint: needed or not; decided by slice A's replay measurement.
2. HMC2 root lock vs batch repair: per-symbol lock, or repair only during a planned
   server restart.
3. Fan-out ring size (60 s / 32 MiB proposed) versus the worst observed server restart
   time; measure in slice C.
4. Whether the capture should also journal its own outgoing subscribe frames and acks
   as kind 1 records (they are today only as incoming acks); useful for audits, not
   needed for rollups.

## Owner decisions (2026-10-02, approved: all defaults)

1. Roller is a library hosted in `sentinel-server` (live) plus the batch CLI `sentinel-roll`; no separate roller service.
2. The product set is fixed by the capture's arguments; the server refuses others.
3. Per-product grids are derived automatically: tick = about 1 basis point of price, rounded to a 1-2-5 step, never below the product's quote increment, fixed per recording day; `recording.products.<id>` may override (BTC keeps its explicit near $1 / deep $5).
4. Rebuilt history goes to a new root `/Volumes/T7/sentinel-data/hmc2` (the 09-28/29 BTC days copied in).
5. Parity = run-to-run byte identity of the roller plus decoded-record parity with the recorder on qualifying minutes (observedMs == 60000, no kResynced, kLateEvents masked); not whole-file byte parity.

## Owner decisions, round 2 (2026-10-03): acceptance after slice A

Supersedes decision 5 (parity). Exact decoded parity with the live recorder is not achievable (independent Coinbase connections; 250 ms timer ticks vs record-time ticks) and is not a goal.

1. **Strict, same journal:** the roller is byte-identical run to run and after a crash-resume on a real-sized journal; in slice C the live roller equals the batch roller per bucket (mismatch metric 0 for 48 h).
2. **Strict, identical input, legacy timer ticks vs record-time ticks** (pinned fixture test): mids, bounds, peaks and row sets exact; per cell |delta TWAP code| <= 32 codes (one code = 0.085% of size, so 32 codes ~= 2.7% of that cell's size; observed max 23); at most 1% of cells may differ at all; total TWAP per side within 0.01%.
3. **Informational only, cross-connection** (daily report, never a gate): per qualifying minute total TWAP per side within 0.5%, |delta mid| <= 0.02% of price, entry count within 1%, row-set overlap >= 99%.
4. **Deep grid:** for products without an override, deep tick = two 1-2-5 steps above the near tick (e.g. ETH 0.5 -> 2, SOL 0.02 -> 0.1); BTC keeps near $1 / deep $5.
5. Slice A is accepted only after the midnight column-loss fix and the live-recorder-root refusal land with tests.
6. No additional snapshot/state-checkpoint machinery for this (no hourly roller state snapshots); the existing journal-snapshot replay and checkpoint stay as built.

## Slice A as built (2026-10-03; baseline 11dee5c, review fixes uncommitted)

Implementation and measurements: [docs/ROLLER.md](../ROLLER.md). Added the
`libs/core/roller` library, thin `sentinel-roll` / `hmc2_diff` bootstraps, shared
L2/trade parsing, per-product daily grids, explicit journal receive clocks,
blocking admission, commit bounds, durable checkpoints and offline regressions.
The always-on services, GUI and live recorder defaults are untouched.

Deviations/refinements from the proposed mechanics:

- The live APIs, injected clock and overflow behavior remain until cutover. New
  explicit-clock overloads and opt-in offline queue/writer modes preserve live
  compatibility; a real captured fixture matches the pre-change recorder/store.
- Deterministic HMC2 resume restores a validated delta base and suppresses exact
  duplicate buckets. The original last-record-wins recovery only guaranteed
  decoded identity; resetting the delta base changes physical bytes after reopen.
- Resume replays the same day-anchor snapshot, preserving floating-point history
  and the fixed daily grid, rather than picking a potentially different later
  snapshot at the checkpoint. Checkpoints retain daily states and are fenced at
  receive-minute changes/EOF, after durable writes; positions are validated.
- Tick rounding uses nearest arithmetic-distance 1-2-5, ties upward, clamped to
  quote-increment multiples. Near/deep share the derived tick outside BTC unless
  explicitly overridden. Reference is the latest snapshot at/before day start,
  otherwise the first available snapshot in the day. These unspecified details
  are deterministic and documented, not inferred from the GUI's display ladder.
- Malformed timestamp fallbacks also receive explicit journal time; otherwise the
  old utility's host-clock fallback would leak into deterministic replay.
- No seven-product backfill into production storage was performed. The requested
  real-day benchmarks wrote only to the worktree's `roll-out/`.

BTC 2026-10-01: 1,816,917 records, 53.745 s, 33,806 records/s, 18,287,442 output
bytes. PEPE 2026-10-02: 458,571 records, 14.646 s, 31,311 records/s, 1,157,235 bytes.
The 17.15-million-record seven-product inventory extrapolates to 9.1 minutes;
budget 10-15 minutes. A measured BTC day replays in under a minute, so an hourly
serialized state checkpoint was not added.

**Correction to sections 4.1 and 7:** minute contents are not unchanged by the
tick schedule. A controlled comparison using the exact same recorded BTC input
and unchanged live API with 250 ms ticks versus the specified record-time ticks
produced two qualifying minutes with 24/4 different TWAP codes (max deltas 11/23),
while mids, bounds, entries and peaks matched. Monotone `advance()` clamps later
backward envelopes to the clock already advanced by idle ticks. `kLateEvents`
masking cannot remove that difference. Independent legacy and capture feeds also
need not have identical batch timing, mids or peaks.

Measured real BTC hour 2026-10-01 16:00-17:00 UTC, both layers: 58 qualifying,
0 matching, 58 mismatching, 2 nonqualifying minutes. Legacy parity is **not
passed**. The prescribed tick semantics and unchanged live behavior conflict
with asserting exact legacy decoded parity. The orchestrator's review follow-up
accepted this finding and requested a pinned same-input tolerance test: exact
mids/bounds/peaks/row sets, <=1% differing TWAP entries, maximum code delta 32,
and <=0.01% decoded total-TWAP delta per side, per qualifying minute. This test
passes for both layers; same-journal byte determinism remains strict. Slice A
still does not authorize service cutover.
The deep hour is nonqualifying because two constituent minutes fail the gate.
A second full BTC day on the final code took 52.348 seconds and reproduced all
three HMC2 files and the checkpoint byte for byte.

Baseline validation: full queued mac-clang build passed; CTest reported 83/83 suites
passed (353.73 s), with Metal-dependent cases explicitly skipped in the sandbox.
All ten new cases pass; fourteen fail-without-behavior mutations were verified
with source restoration, touch and rebuild between runs. No GPU/visual result is
claimed. Baseline was committed by the orchestrator as `11dee5c`.

Review fixes keep applying journal records until a drained fence confirms the
recorder committed through the day/range end (or EOF). Receive time can lead its
envelope-based integration clock, so reaching end+lateness in receive time is
not a commit proof; stopping there drops the last pending minute/hour. CLI preflight
refuses configured live roots and product trees containing HMC2 without a roller
checkpoint. Sequence tracking recovers after discontinuities; stop broadcasts
wake blocked offline producers. Real-fixture crash resume verifies restored
large delta bases. Diff JSON adds signed code-delta histograms and decoded TWAP
totals/deltas by side. Tests use build-directory temporary output and clean up.
The pending deep-grid change is deliberately not implemented. Review fixes
remain uncommitted for the orchestrator.


Review-fix validation: full queued build passed; 83/83 CTest suites passed in
340.82 seconds (Metal-dependent cases skipped). All 16 roller cases and nine
review-specific fail-without-fix checks passed after restoration and rebuild.
Controlled comparison worst cases: 0.523218% differing entries, 23-code maximum
absolute delta, and 0.000140795% maximum absolute total-TWAP delta per side;
mids/bounds/peaks/row sets exact. See docs/ROLLER.md for per-minute results.

## Slice B as built (uncommitted lieutenant hand-off)

`capture/CaptureFanout` serves the default-enabled Unix socket, a 60 s / 32 MiB
per-product ring, exclusive-cursor resume with explicit journal-gap boundaries,
and product-scoped resnapshot control (10 s rate limit). A consumer opens one
socket per product. Eight bounded client slots, each 16 MiB, and separate bounded
SPSC ingress queues keep socket backpressure out of the capture QueuePool and
writer locks. All fanout accounting appears on the existing capture endpoint;
the Prometheus capture job already exists. `scripts/dev/fanout-tail.py` is the
orchestrator's position/control probe. Full protocol: `docs/RAW_CAPTURE.md`.

The position type is `capture::JournalPosition {product, runId, block, record}`.
It intentionally has no dependency on unlanded slice A. Slice C should map it to
`roller::JournalPos` (or consolidate the plain DTO at that merge point); RAWL2
framing and coordinates are unchanged.

**Append-time publication (orchestrator follow-up to a616ffe):** the writer assigns
final positions and publishes exact framed records as provisional before any
append-related disk I/O, including the previous block's flush at a boundary.
Successful flushes send inclusive `durable{product,through}` watermarks; failures
send `retract{product,after}` (null before the first successful flush) before the
existing recovery path. Recovery segments do not reuse published positions.
Rings retain provisional records and remove retracted suffixes. Record/control
ordering is preserved, but a watermark can cover a prefix of already delivered
provisional records. Consumers checkpoint only fully applied durable positions,
discard withdrawn provisional state, and resume from the journal after retract
or EOF. The handshake tip includes the current durability ceiling for replay.
Default fsync remains one per block; custom weaker durability settings retain
their power-loss exposure. This replaces the earlier after-flush limitation;
live handoff no longer waits for the appended record's disk operation. The
existing disk-worker queue can delay entry to append, so slice C must still
measure full feed-to-GUI age before claiming the +50 ms acceptance gate.

The required ring retains bytes with zero clients, so literal zero memory
overhead is impossible. Payload capacity is not preallocated. Benchmark and
validation results are recorded below. No service was deployed,
restarted or stopped; production data was not written.

Fifteen fanout cases pass, alongside RAWL2 and capture application/lifecycle
suites. The five new cases cover provisional delivery before initial disk I/O,
durable visibility and cross-block ordering/replay, real short-write retraction
with consumer rollback, null retraction/position non-reuse, and immediate Session
fault notification before close. All 20 fail-without checks pass, including the
original 13 and seven new checks: premature durability, missing provisional or
durable notification, missing writer/Session retraction, stale ring suffix and
position reuse. `tests/capture/fanout_mutations.py` restores, explicitly touches,
rebuilds and re-passes each regression after every mutation.

Overhead method: `capture_fanout_benchmark baseline|idle|client 65`, separate
process for each mode, 100 records/s x 1,500 payload bytes, real Session/Writer
with default fsync cadence and temporary internal-disk output. `idle` means
fanout enabled with zero clients; `client` drains every record in a local reader
thread, so its CPU/RSS numbers conservatively include the consumer too. The run
lasts longer than the 60 s retention window. This measures the fanout addition
to disk capture; it does not include Coinbase, TLS or engine JSON parsing and
is not a deployed-service CPU claim. Literal zero-client zero-memory overhead
cannot coexist with retaining a replay ring.

Full queued mac-clang build passed; **84/84 CTest suites passed, zero failures,
351.10 s**. The fanout suite passed all 15 cases in 1.45 s. Metal cases explicitly
skipped because the sandbox has no MTLDevice; no GPU/visual verification is claimed.
All 20 mutation checks restored, touched, rebuilt and passed. Validation uses
branch base `a616ffe`; main had advanced to `9da1845` at hand-off. Fixes remain
uncommitted, with commit/rebase/integration left to the orchestrator (Git metadata
is read-only in this worktree's sandbox).

Append-time measurements on the owner's Mac, 2026-10-03, sequential queued
65 s runs (6,500 frames plus one terminal stop per run):

| Mode | CPU seconds | Average CPU (one core) | Peak RSS bytes | Received records |
|---|---:|---:|---:|---:|
| Writer-only baseline | 0.368127 | 0.566349% | 17,678,336 | n/a |
| Fanout, zero clients | 0.747469 | 1.14995% | 29,999,104 | n/a |
| Fanout, one draining client (reader included) | 1.26058 | 1.93935% | 30,720,000 | 6,501/6,501 |

**Append -> client receive: p50 117.958 us, p95 254.709 us, max 3,072.67 us**,
6,501 samples. The benchmark timestamps entry to Writer append through complete
socket packet receipt, before consumer JSON parsing; it excludes earlier Session
queue dwell. This meets sub-millisecond p95 handoff in this fixture; it does not
measure full exchange-to-GUI age or the deployed seven-product workload.

Zero-client overhead is +0.583603 percentage points CPU and +12,320,768 peak RSS
bytes (11.75 MiB). This is measurable: the literal "no measurable overhead" claim
is not made. Ring accounting was 10,833,462/10,833,464 bytes (~10.33 MiB), oldest
age 59.994/59.993 s. Both fanout runs ended with zero ingress bytes, ingress drops,
client-queue bytes and disk-pool bytes; no unintended disconnects occurred.
These replace the earlier after-flush measurements. They are single-run results,
not confidence intervals, and one-client CPU/RSS includes the reader thread.

Deploy watch list (orchestrator only):

1. Review and land, build main, then use `scripts/dev/deploy-runtime.sh capture`
   with the owner present. Verify private runtime/run directory, socket ownership,
   `sentinel_fanout_running 1`, all seven feed gauges, and continued RAWL2 writes
   inside the deploy script's 60 s window. A bad/occupied socket path refuses
   startup rather than silently disabling fanout.
2. Tail positions with `fanout-tail.py`; reconnect inside and beyond retention,
   checking hit/miss counters and the explicit journal boundary. A full seven-
   product consumer uses seven sockets, leaving one of eight slots for a probe.
3. Stall a disposable consumer: check a `slow_client` disconnect, flat ingress-
   drop counters, and uninterrupted writer/other-consumer progress. Watch ring
   RSS at the actual seven-product rate; fanout memory is additional to QueuePool.
4. Intentionally request one resnapshot and verify that only that product changes
   connection, the journal contains its resync/snapshot, and a second request
   within 10 s is rate-limited. This is an explicit operator action, not a passive
   health check.
5. Verify provisional records arrive between flushes and durable watermarks
   advance after flush. Monitor p50/p95 handoff and full feed-to-GUI age in slice C.
   Exercise failure/retraction only on an isolated fixture, never by faulting the
   always-on capture. A consumer must discard its provisional suffix on retract
   or EOF and recover from its durable checkpoint. No deployment/live feed check
   was performed by this branch.

Workflow notes: CMake regeneration initially tried to lock the read-only shared
vcpkg checkout; configured this build with `VCPKG_MANIFEST_INSTALL=OFF` using the
already installed dependencies. Shared ccache writes are also sandbox-blocked;
builds used `CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp`. The shared FIFO queue was the
largest wall-time cost. This branch's plan lacked "Slice A as built"; that section
was read from the `lt-astra/roller-a` branch without depending on its code.
