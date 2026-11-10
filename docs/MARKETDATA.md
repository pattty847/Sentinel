# Market Data Architecture

## Overview

In capture, `MarketDataFeeds` owns one shared Boost.Asio `io_context`, one `mdc-io` thread,
one TLS context (CA loading once), and one `MarketDataCoreEngine` per product.
Each engine owns one transport and strand, a single connection sequence counter,
its own heartbeat/L2 liveness and reconnect state. Core depends on QtCore logging,
never GUI Qt. Consumers retain history and books; engines only dispatch data.

Coinbase Advanced Trade `market_trades.side` names the resting maker. The
`MarketDataCoreEngine` flips BUY to sell aggressor and SELL to buy aggressor
once for engine consumers. The server consumes `JournalFeed`, which independently
flips the raw maker side once; `JournalTradeEventSource` does the same for backtests. Unknown stays unknown.
Server footprint delta is positive for buy aggressors and negative for sell
aggressors; candle, TPO, and volume profile calculations ignore side. Live
stream `trade` messages carry `side_basis: "aggressor"` so clients use `side`
without another flip. An unmarked older stream carries Coinbase maker side.

The server owns no exchange engine. Live books, metadata, trades and candle
clocks come from the capture fan-out and journal through the roller; REST is
used for candle history. Legacy binary tick writers and readers are removed;
backtests read RAWL2 journal trades or explicitly imported CSV files.

```
Coinbase -> capture -> RAWL2 journal + fan-out -> roller -> queued ServerDataModel -> clients
```

## Connection ownership and isolation

`add(product, pinned=false)` returns `Added`, `AlreadyPresent`,
`CapacityExceeded`, or `InvalidProduct`. A duplicate add opens no connection.
`remove(product)` removes admission state and queues only that socket for close,
returning without waiting for transport-down. Completion is logged; owner stop
drains retired sockets too. A retired socket's errors never reach the error
callback, and its log lines read `retiredProduct=<id>` (engine) or
`product=retired:<id>` (transport), because a re-added engine may already own
`product=<id>`. No unsubscribe frame is sent. Pinned feeds cannot be removed. Engines send exactly three
subscribe frames after transport-up: level2 and market_trades with their one product,
and connection-scoped heartbeats without product_ids. Subscribe batches have their own process-wide admission deadline, so delayed
handshakes completing together cannot bypass message pacing. A product that is
reconnected while its subscribe batch waits in that queue leaves the queue at
once, so it cannot hold other products' subscribes behind its closing socket
(Coinbase drops a socket with no subscribe within 5 s).

Sequence gaps, malformed messages/L2, provider errors, transport failure, inbound
silence, and consumer resnapshot requests invalidate/reconnect only the affected
product. Invalidation is ordered before later data callbacks; updates are withheld
until a fresh snapshot is accepted. Unexpected product IDs are protocol errors,
never forwarded to another product's consumer. Invalidation callbacks always name
a product. Acks warn when an explicit level2 list omits that product; a trades-only
ack makes no level2 claim. Acks never establish book validity.

Each failed connection retries with exponential 1 s..30 s backoff, reset by
the first accepted L2 snapshot, not a successful handshake. Persistent provider
errors and malformed first messages therefore retain exponential backoff. Duplicate down notifications share one pending retry. A connected
stream with no inbound message for 20 s reconnects with at least 5 s backoff.
L2 silence initially triggers at 30 s, even while heartbeats/trades flow. It
invalidates and reconnects that product directly. Its silence threshold doubles
to 10 min across reconnects and quiet snapshots, resetting only after a snapshot
followed by a nonempty valid update. There is no resubscribe dance, peer-health
heuristic, escalation cap, or snapshot fingerprint. Consumer resnapshot requests
have a separate per-engine 20 s cooldown.

The owner drives connection and watchdog deadlines with one 100 ms Asio timer;
each engine applies its own deadlines on its strand. A monotonic clock and jitter
source are injectable, with manual pumping for deterministic offline tests.
No additional thread or per-frame timer is created. Beast enforces separate
connect/close deadlines (`ServerMdcConfig::connectTimeoutMs`, default 20 s;
`ServerMdcConfig::closeTimeoutMs`, default 3 s). Pending transport operations retain the
transport through cancellation, so removing a feed cannot leave dangling handlers.
Owner shutdown closes every socket before joining the producer thread.

## Connect pacing, capacity and diagnostics

Every initial/reconnect attempt draws 0..1000 ms jitter in addition to its
backoff. Two process-shared capacity-one token buckets independently admit one
connect attempt and one three-frame subscription batch per second. Neither
accumulates burst credit. Subscription batches are at least 1,000,000 microseconds
apart even if delayed handshakes complete together; failed handshakes consume only
connect admission. FIFO tickets prevent a failing product from starving peers;
closing a feed cancels its pending tickets. Server and capture together send at
most six unauthenticated subscribe frames per second, below Coinbase's eight
messages/s/IP budget, and at most two connects/s. Scheduling adds up to 100 ms
polling quantization plus event-loop work to deadlines. These budgets cover the
two Sentinel processes; unrelated programs sharing the IP need their own allowance.

`MarketDataFeeds::Options::maxConnections` caps non-pinned feeds; zero means
unlimited. Pinned recorder products never consume that allowance. Exceeding it
creates no engine/transport, evicts nothing, returns `CapacityExceeded`, and
logs `sLog_Error` with `symbol`, `cap`, and `code=capacity_exceeded`. Slice 3
will configure the server default of 8 and surface refusals to clients/status;
this core slice leaves the server unlimited.

Engine and production transport diagnostics carry `product=` and `conn=`.
Connection IDs increment only on transport-up, independently per engine. Failed
attempts do not consume IDs or count as reconnects. A separate `attempt=` counter
identifies all attempts in logs and the `feeds.connect` probe.
`stats()` returns each product's up/down state, connection ID, reconnect count,
sequence, last-message age and L2 age. The owner logs aggregate/per-product stats
every 60 s; `ws.rx` identifies raw frames and `feeds.connect` reports bucket wait.
A product continuously down for two minutes emits `sLog_Error`, repeating at
most once per minute until up. The pre-parse ingest observer includes product and connection on every event.

## API and threading

Lifecycle calls and callback setters belong to the owner's non-I/O thread.
Set callbacks before the first successful add; later setters throw `logic_error`
instead of silently leaving existing engines unchanged. `requestResnapshot(product)` is thread-safe,
including from capture supervision. Lifecycle operations synchronize with the I/O
thread; consumers must not call blocking lifecycle/stats methods from callbacks.
All engine state changes enter its strand. Transport callbacks, JSON parsing and
DTO callbacks run on the single I/O thread; server/model notifications cross to
Qt with `Qt::QueuedConnection`. `MarketDataCoreQt` and its unused service-locator
entry points have been deleted rather than preserving a second upstream API.

The per-engine update vector is reused; snapshots allocate their own level arrays.
`MessageDispatcher` parses provider errors and subscription acknowledgements.
L2 parsing validates side, positive finite price and nonnegative finite quantity.
`Authenticator` supplies optional JWTs; public market-data channels need no key.
`LiveOrderBook`/recording consumers own book storage, caching and persistence.

## Server integration boundary

Capture owns `MarketDataFeeds`. The server follows the journal through roller
workers, with queued book, trade, metadata and health hand-offs to the model.
Subscriptions select captured products and release only their own client views;
they never acquire an upstream feed or start, stop or flush a recorder. The roller
owns each product's recording lifetime and its per-product stall deadline.

Recording resnapshot requests: only the serving roller (`recording.source: roller`)
sends capture's fan-out `{"type":"resnapshot","product":P}` command, on that
product's own subscribed connection and only at the live socket tip, never during
replay or a handshake (`ResnapshotPolicy.hpp`). A request is due only when all of
these hold: the day's history recorder reports a wait
(`BookRecorder::snapshotWait(P)`; no history recorder yet also counts, as "no
usable snapshot"); 10 s have passed since both the wait began and the newest
connection change in the history feed (TransportUp, TransportDown/CaptureStopped,
a new run or connection id, a journal boundary, including after a damaged block);
the history feed applied a durable record for P at the live tip within the last
5 s; no provisional connection transition (a lifecycle record, or a new
connection or run) is still waiting in the fan-out for history to apply it; and
the backoff deadline passed. Recent records are activity evidence only, not
proof that the connection is ready: a down or reconnecting product journals
lifecycle records but usually no frames, every lifecycle record is a connection
change that restarts the grace, and the grace stays longer than the window
(validated). Capture's engine would ignore a request while it reconnects. The wait's provenance (the recorder's own
invalidation, a continuous `unobserved suspect` book, an anchor imported invalid,
or an upstream/journal invalidation) is logged but does not change the timing.
An accepted snapshot ends any wait. A precautionary unobserved wait also ends
when continuous updates restore a two-sided, uncrossed, representable midpoint.
A replay that ends with broken continuity still needs a snapshot and asks from the tip.
The next request follows after 30 s, doubling after each `forwarded` reply up to
10 min (refused replies keep the delay); a connection change restarts the grace
before a retry; after 60 s with a valid book the delay and any pending deadline
reset. Each written request logs `Recording resnapshot request: symbol=...
reason=...` and counts `sentinel_recorder_resnapshot_requests_total{product}`;
each reply logs `Recording resnapshot reply: symbol=... status=...`;
`sentinel_recorder_resnapshot_waiting{product}` is 1 while the live roller's book
waits and 0 off the live tip. Shadow-only runs, the hourly oracle and offline
replays never send one. The model's await-snapshot paths use `requestReseed` (a
synthesized roller snapshot), never a capture resnapshot.


## Heatmap timeframes

`server_config` schema 2 advertises `chart.timeframes_ms` (the configured
candidate list), optional `chart.default_timeframe_ms`, and derived
`chart.served_timeframes_ms`. The served list contains every configured
timeframe the candle handlers answer: 100 ms and 1 s from memory, multiples of
60000 from REST; the default does not narrow it. The toolbar disables every
choice outside this list, 100 ms included. An absent served list means unknown
availability. Recording capability retains its
minute/hour-alignment filter in `recording.timeframes_ms`. Server and client use
schema 2 together; the retired server `heatmap` fields have no fallback aliases.

Candle timeframe identifiers are millisecond durations internally and `timeframe_ms` on the wire. The 100ms policy draws no trade-free buckets and synthesizes no quiet bars; whole-second behavior is unchanged. For live aggregation, the 1m candle is the anchor for every configured coarser candle (5m, 15m, 1h, 4h, 1d). Rollups use the first open, last close, extrema of highs/lows, and sums of volume and trade count. Live 1m empty bars carried by the server are included in coarser bars. A coarser bar's final value is built from closed minutes only; while it forms, each accepted trade publishes it including the open minute (one event per trade for all coarser timeframes), so it appears at its period's first trade and its last forming value equals the closed bar. Live updates pass a per-session, per-timeframe gate (price move, new high or low, volume, silence; `server_config` candle update fields). For 1m and coarser, the first traded update of a bucket bypasses those thresholds: a new bucket with a trade, or a sent empty (carried) bar of the same bucket gaining its first trade; later updates of the bucket stay gated. Historical REST backfill uses Coinbase native candles (1m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 1d). Other configured minute-multiple timeframes roll up the largest native divisor on UTC epoch boundaries. REST does not provide trade count, so backfilled counts remain zero. REST history includes only buckets closed when the fetch begins; the forming bar remains owned by the live aggregator. The REST history wire range is half-open, translated to Coinbase's inclusive end by subtracting one second before the provider applies its 350-bar cap.

The server caches settled closed REST history in memory, keyed by product and requested timeframe, retaining at most 16 least-recently-used series and the newest 8,192 bucket slots per series. A bucket becomes cacheable only when its end is at least one native source bucket (minimum 60 seconds) behind the fetch start time. Recently closed buckets are returned but remain uncached so later requests can correct missing or partial provider results. Settled empty intervals consume slots too; errors and still-forming buckets are never cached. Covered settled requests make zero REST calls. Identical in-flight missing ranges share successful results; a failed or abandoned producer releases the flight so other sessions can retry using their own cancellation state. Other cache hits do not wait for provider work. Cache misses start at most one REST request every 400 ms (2.5/s across candle-history sessions). Pages within a request remain sequential; separate misses may overlap provider I/O. Non-native output pages may need multiple native provider pages. No disk cache is written.

Candle history follows the visible time range with one additional screen of older prefetch. Requests use a leading/trailing 100 ms throttle: send immediately when idle, then keep the earliest trailing deadline during sustained movement. Each page requests at most 350 bars at the selected timeframe, newest first. Pages continue across the visible boundary into prefetch, filling up to 350 bars per REST call. This also applies to refresh passes. The client serializes candle-history requests across selection changes, matches replies by symbol/timeframe/start/end, and rejects previous selection generations before merging. History merges on the GUI-owned candle buffer, never in QSG callbacks, without advancing the live sequence. A bucket owned by the live stream always wins over history, even when a REST snapshot was labelled closed after crossing a boundary during its fetch; live updates can also take ownership of a previously closed history-only bucket. Reconnect clears ownership and the live sequence counter for every cached series; selecting a cached symbol/timeframe again does the same for that series. These cached bars become eligible for a history refresh until new live updates establish ownership. Reconnect and cached selection re-entry queue one newest-first refresh pass covering the visible range plus prefetch, even when the oldest cached bar already covers that range or the cache is full. The pass uses the normal page caps, throttle and single-flight guard; after its pages finish, older backfill resumes. Live candle deliveries carry a local client generation captured before their queued handoff. The data source rejects mismatched generations, symbols and timeframes so a queued pre-reentry bar cannot reclaim ownership after an A-B-A switch. This tag does not change the wire protocol.

An empty or overlap-only successful page advances the scan cursor to the previous window; it does not prove that older history is exhausted. Three consecutive empty windows pause that symbol/timeframe for 60 seconds. While the viewport still needs older data, a timer resumes from the saved scan cursor, including for a stationary viewport; it never restarts at the loaded edge after a pause. Empty 100ms and 1s scans have a seven-day total time lookback from their first empty window, preserved across pauses; REST timeframes have no additional empty-lookback cap and remain paged and paused. Finding older bars starts a new empty-scan budget. Returning to loaded coverage, filling local capacity, or exhausting the memory-history lookback cancels the retry timer. Transport errors preserve the cursor and retry the same window after 2 seconds. A `warming` reply is not a page: the client holds that request (no cursor moves, no empty page is counted, no pause starts) and resends it verbatim 2 seconds later while the viewport still needs older data; a symbol or timeframe change, or a reconnect, drops it at once. The chart shows no warming label; live bars keep merging meanwhile. Backfill stops at the existing local retention capacity (60,000 bars at 100ms and 1s, 20,000 through 1m, 10,000 coarser), so pages cannot loop after immediate cache eviction.

Candle schema version 2 carries timeframes and history boundaries in integer milliseconds. The server's configured 100ms and 1s history contract is an inclusive `end_time_ms` with `limit` counting retained bars (capped at 350), drawn from per-timeframe RAM retention: a timeframe with a `chart.history_bars` entry keeps between the value and the value + 10 percent closed bars once populated, and a timeframe without an entry keeps the 10,000-bar cap (9,001..10,000 once populated). The checked-in configuration serves 100ms with an entry of 18,000 (30 minutes of contiguous buckets) and 1s with no entry. After a server restart each captured product rebuilds, from the capture journal at the first handshake and before any live trade reaches the model, the newest 30 minutes of its 100ms and 1s bars (their window needs at most 4 journal files) and the open bar of every served timeframe from 1m to 1d (the open 1m bar and each coarser bar's closed minutes, built exactly as the live aggregator builds them; closed 1m and coarser bars stay REST-served and are not installed). The bars come from exactly one continuous replay from a fixed position: the journal file holding the earlier of the window start and the oldest open bucket start (UTC midnight when 1d is served), less one minute of lead-in and a 2 h safety margin, clamped to the journal's start and to 32 files (4 without open bars). The margin lets the continuous read converge on the uninterrupted path (carried bars, delayed trades across file boundaries) before the first bar that matters; the bars are installed only into empty series, so each open bar equals the bar an uninterrupted server would hold. There is no extension or second read: when the replay holds no 1m bar before the oldest open bucket, or no 100ms / 1s bar before the window, the outcome is `partial`. When the cap leaves out a journal file whose header shows the receive clock past the rebuilt bars and past the replay's first file (a clock regression the uninterrupted aggregator saw), no exact result exists: the rebuild fails (warming ends, nothing is installed, the failure is logged and counted). Its cost grows with the time since that bucket began plus the margin (measured on 2026-11-07 recordings: BTC-USD about 0.9 s per journal hour, about 25 s just before UTC midnight, 7 products in parallel peak about 120 MB RSS); the product's live feed and journal roll start after it. Live delivery continues from exactly the record after the rebuilt one, and reconnect resends of a rebuilt trade id are dropped until 16,384 new nonempty ids have been delivered. An open bucket that began before the journal's oldest readable record (or beyond the 32-file cap), or a thin product with no trade in the margin, holds only the trades the replay has; nothing is synthesized for the rest, no empty bar is installed, and the rebuild logs and counts `outcome=partial`. Timeframes longer than 1d are not rebuilt: their open bar starts with the first live minute. Until that rebuild ends, a 100ms or 1s request for the product is answered with a `candle_history_chunk` that carries `status: "warming"`, an empty `candles` array and the echoed `symbol`, `timeframe_ms`, `start_time_ms` and `end_time_ms`; 1m and coarser requests never warm, and a ready product never sends `status`. A rebuild that fails or cannot establish its initial state inside the file caps still ends warming: the product serves an empty window and live-only open bars (failure) or bars from its first trade the read holds (`partial`), and the server logs and counts it. Live bars flow during warming. Nothing is persisted. For sparse series, returned bars may predate the nominal `start_time_ms = max(0, end_time_ms - timeframe_ms * limit)` metadata; that field is not a lower filter bound. The client pages backward with `end_time_ms = oldest_loaded_ms - timeframe_ms` to exclude overlap. Coarser history uses REST time windows.

### Live flow bars

`server_config.flow` announces `schema_version: 1`, `live`, `encoding: "FLB1/base64"`,
`notable_k: 8`, `live_publish_ms`, `close_delay_ms: 1000`, and
`second_retention_ms: 1800000`. `history: true`, `history_encoding: "FHE1/FLB1"`,
`history_max_bars: 512`, and `history_second_replay_max_ms: 3600000` announce flow history. `grid: "per_bar"` means that `row_tick_units` and `price_scale`
come with each bar: the recorder's grid is specific to the product and UTC day,
so there is no single global grid in the connection handshake.

A client sends
`{"type":"flow_live_subscribe","sub":7,"symbol":"BTC-USD","timeframe_ms":60000}`.
`sub` is an unsigned 64-bit connection-local identifier. Reusing it replaces the
previous subscription. Supported timeframes are 1 s, 1 m, and configured chart
timeframes from 1 m through 1 d that divide a UTC day exactly. Sub-second flow is
refused. There are at most 32 subscriptions per connection and 256 in the model;
invalid requests, unsupported products/timeframes and capacity refusals return
`error` with `context: "flow_live_subscribe"`. Subscription does not admit a new
upstream product: it uses the configured journal products.

The server sends JSON `flow_bar` with `schema_version`, `sub`, `symbol`,
`timeframe_ms`, `bucket_start_ms`, `closed`, `complete`, `revision`,
`source: "live"`, `row_tick_units`, `price_scale`, `encoding: "FLB1/base64"`,
and `bar` (base64-encoded FLB1 bytes).
FLB1 contains OHLC, integer base/quote volumes and scales, count, native price
rows, buy/sell totals, delta, maximum row volume and up to eight notable trades.
Revision is **only envelope metadata**, never part of FLB1 content. A forming
update and close republish can share a live revision. Coverage begins when the server
receives its first live trade; a bar is complete only if that trade was at or
before the bucket start and no trade in the bucket was lost or rejected. An
empty minute is covered, not a loss. Every accepted late
trade behind the published close watermark increments the revision; multiple
late trades can coalesce into one publication with a correspondingly larger revision.

Trades use exchange-time, UTC-aligned half-open buckets and the shared
`flow::FlowBarBuilder`. Its 4,096-id window protects against reconnect resends;
unknown aggressor side contributes to total volume/count only. The native grid
is supplied by the recorder for that UTC day, with decimal base-increment
precision and a quote-volume scale of 100,000,000. The existing TPO/VP tape remains.
The live model starts at the first live trade after process start. It builds
only 1 s and 1 m bars from trades. It composes 5 m, 15 m and 1 h bars from
retained 1 m bars, and 4 h and 1 d bars from closed-hour composites plus the
forming hour. Closed-hour composites are retained for the UTC day and its
60-second rollover grace, at most 24 per product; the forming hour is separate.
A coarse composite is recomputed when a constituent minute changes, its
coverage changes, a subscriber attaches, or its bucket closes. No journal
rebuild or historical coverage is implied by these live messages.

The model's Qt-thread timer materializes and encodes changed bars at most once
per `recording.live_publish_ms` (clamped to the configured 250–5,000 ms range).
A newly attached subscriber receives the newest retained bucket on its next
turn. During midnight rollover, this may include the newest slot of both the
preceding and current UTC days. A final close is republished once even if the
content did not change.
The product clock is its monotonic journal receive watermark minus 1,000 ms;
wall time and quiet/disconnected intervals do not close bars. Publication
precedes `sealThrough`; late trades remain revisable until retention drops them.

The 1 s series keeps the last 30 minutes, and the 1 m series keeps the forming
UTC hour with 60 seconds of rollover grace for the preceding hour. A subscribed
coarse view keeps its forming composite and the preceding composite until 60
seconds after close. Fixed slots cap each day/series at 1,802 second buckets
and 62 minute buckets, including clock/publication slack. Only the
current and preceding UTC day can be held. Watermark maintenance drops only
already-published, unchanged finals; a stalled publication loop refuses additions
that would overrun the slots. `sentinel_flow_dropped_trades_total` counts additions
refused behind the retention watermark, at capacity, for missing grids or by
builder validation. `sentinel_flow_dropped_buckets_total` counts retired buckets. The sibling
`sentinel_flow_missing_grid_trades_total` counts trades for a UTC day without
a recorder grid; the server warns once per product-day.
`sentinel_flow_hour_shrink_refusals_total` counts attempts to replace a cached
hour from fewer source minutes. Dropped-trade counts are per series, so one
input trade can affect several series. Retention bounds the time
window and bucket count, not the exchange's trade rate within a bucket.

`{"type":"flow_live_unsubscribe","sub":7}` cancels the subscription. Cancellation
and replacement discard queued frames; an already in-flight WebSocket frame can
finish. Disconnect also cancels registrations and publication. The stream I/O
thread only admits requests and sends prepared messages; model-thread aggregation
and FLB1/base64/JSON encoding never run there. Flow uses the existing bounded
16 MiB session write queue and slow-client disconnect policy.

### Flow history

`{"type":"flow_history_request","req":9,"symbol":"BTC-USD","timeframe_ms":60000,
"end_time_ms":1798765500000,"limit":512}` requests the newest page ending at the
UTC bucket containing `end_time_ms` (inclusive). All fields are required; `req`
is an unsigned 64-bit connection-local correlation identifier. Timeframes are
1 s, 1 m, or configured whole-minute multiples through 1 d that divide a UTC day.
Sub-second requests, negative/overflowing times and limits outside 1..512 are
refused. There is one flow history job in flight per connection, including the
queued model snapshot. A concurrent request or saturated shared history pool
returns JSON `error` with `context: "flow_history_request"`, `req`, `symbol` and
`message`; it never returns a successful truncated page.

An accepted request returns one binary WebSocket message:

| Bytes | Meaning |
| --- | --- |
| 4 | ASCII `FHE1` envelope magic |
| 4 | Little-endian uint32 JSON header byte length |
| header length | UTF-8 JSON header |
| repeated for each header `bars` entry | Little-endian uint32 FLB1 byte length, then that many FLB1 bytes |

The header contains `type: "flow_history_chunk"`, `schema_version: 1`, `req`,
`symbol`, `timeframe_ms`, `price_scale`, `row_tick_units`, `status`, `oldest_ms`,
`sealed_through_ms`, `ranges` and `bars`. `bars` entries carry `bucket_start_ms`,
uint32 `revision`, boolean `complete`, and `source: "store" | "live"`, in the
same ascending order as the binary records. The watermark is the store's
contiguous built-through hour for that symbol (every preceding closed hour is
built), rather than the recorder's committed `hourThrough`; it refreshes on
every page. A store read can still report `building` below the watermark if a
file becomes stale or unreadable after the mark advances.
Only a bucket ending at or before that boundary is final and served as `store`.
A coarse bucket crossing it is provisional (`source: "live"`, `complete: false`):
its FLB1 composes sealed store hours with the live forming part using the shared
composer. Its revision is the model's live coarse revision if present, otherwise
zero. Missing constituent hours or minutes also force revision zero. The
incomplete flag lets a live update at the same revision replace it.
For a final bucket with partial or building store coverage, an available bar
is `source: "store"`, `complete: false`, with its store revision; a later
complete store page replaces it by store revision.
Store revisions use the FLD1 day generation, including empty constituent hours.
Revision and completeness are envelope metadata; FLB1 content is unchanged.
Each range has half-open `from_ms`, `to_ms`, `status`, `price_scale` and
`row_tick_units`. There is one range per requested bucket, including buckets
with no trades. Top-level scales describe the first returned bar (zero without
bars); each FLB1 and range carries its own grid, which can change across UTC days.
The whole frame is capped at 15 MiB; excess returns a correlated error asking
for a smaller limit.

Range statuses are:

- `complete`: coverage is proven, including known empty buckets. A forming
  bucket is complete through the snapshot's observed journal clock if coverage
  began at or before its start and no loss is known.
- `partial`: only part of the requested coverage is available (for example a
  restart inside a minute, a short journal or rejected trades). Returned bars
  have `complete: false`.
- `building`: some required store hours or live coverage are not built or are
  stale/corrupt. Available constituents may be returned with `complete: false`;
  no empty complete bars are manufactured. Clients show the range as not built.
- `refused`: invalid request or cap violation. The binary reply includes `error`
  naming the reason and no bars; its range has `from_ms`, `to_ms` and `status`.

A range can report complete coverage while a crossing bar is still marked
`complete: false` because it is provisional. The page status is `building` if
any range is building, otherwise `partial` if any range is partial, otherwise
`complete`. `oldest_ms` is the oldest requested bucket, including empty or
building ranges, so paging can always make progress.
Fetch the next older page with `end_time_ms = oldest_ms - timeframe_ms`, stopping
at zero. Pages select the newest interval first; bars within each page are
chronological. Clients should retry building ranges separately after catch-up.

Minute/coarse requests read only the store's sealed-hour index and FLD1 files,
plus a queued read-only copy of the model's retained minute series for the
forming hour and times newer than the built-through watermark. Live minutes
and closed-hour composites remain available until the store builds them, up to
six extra closed hours; further lag is logged and appears as `building`.
Minute and coarse requests never replay or scan journals. Coarse bars use
`flow::composeFlowBars`, the same composition and
scales as live. Missing sealed hours remain building until the background store
builder catches up. After a restart, missing unsealed minutes remain building;
live coverage starts with the first observed trade and cannot prove an earlier
prefix. Empty observed minutes are covered without fabricating trades.

The built-through watermark scans contiguous indexed hours from the live
retention floor (the journal clock less the one-minute grace and six closed
hours). An older unbuilt hour remains `building` in its own range but does not
hold the watermark behind the live fallback window. On restart, committed day
indexes establish the watermark before new hours finish building. Repeated
hour-build failures log the symbol, hour and reason.

Second bars use the retained live 30-minute series. Older portions use a
worker-local, one-shot journal replay with no persistent writes. The replay
API has a 3,600,000 ms (1 h) defensive window cap; the 512-bar page limit is
stricter for server requests. A two-hour 1 s request is refused with both limits
named in its error. The time cap does not bound replay work to one hour of
records: a request may read one hourly journal segment plus the requested
window. Journal replay uses the same grid resolver, trade builder and receive-time
tail as the store. A missing/short journal returns partial coverage. A
second replayed inside an unsealed hour remains provisional (`source: "live"`).
Seconds inside the 30-minute RAM retention are served from RAM without a
journal read. A covered second ending at or before the built-through watermark
is labelled `source: "store"` and final; older seconds use replay. A
RAM-served final second uses store revision zero so a later journal replay with
a store generation can replace it. Only one
replay runs server-wide at a time. A concurrent replay gets a `busy` error;
session close or server stop cancels an active replay per record.

### Client merge of flow bars

Key bars by `(symbol, timeframe_ms, bucket_start_ms)`. A final sealed
`source: "store"` bar replaces a `source: "live"` bar regardless of revision;
live never replaces store. Compare revisions only within the same source: the
higher store generation wins over an older store generation (at equal generation,
complete wins over incomplete), and the higher
live revision wins over an older live revision. At equal live revision,
`complete: true` wins; otherwise apply live updates in stream order. The
history chunk's `sealed_through_ms` replaces the client's prior watermark for
that symbol. Drop a later live `flow_bar` only when its bucket end
(`bucket_start_ms + timeframe_ms`) is at or before that watermark. A coarse
bucket crossing the boundary remains live and accepts later live updates. Range
status stays separate: show `building` as not built even if a provisional bar
exists.
No client disk cache is required.

## Heatmap history transport

History uses binary chunks requested by `heatmap_chunk_request`, with the raw
live tail on `heatmap_live_subscribe` (contracts below). The legacy HMCL
`heatmap_slice`, JSON/base64 history pages and band-projected live views are
removed. The server never opens or changes existing `data/heatmap` archives.
The bounded history worker pool remains shared by chunks, candle history,
trade overlays and screener work; shutdown joins it before destroying I/O.

## Volume profile stream

`volume_profile_slice` carries `schema_version` for the volume profile family. Session boundaries are Unix milliseconds in `session_start_ms` and `session_end_ms`. The client also accepts legacy `session_start` and `session_end` fields when the corresponding `*_ms` field is absent; the suffixed fields take precedence.

## File layout

```
libs/core/marketdata/
├── MarketDataFeeds.hpp / .cpp
├── MarketDataCoreEngine.hpp / .cpp
├── ws/           WsTransport, BeastWsTransport, SubscriptionManager
├── auth/         Authenticator
├── dispatch/     MessageDispatcher, Channels
└── model/        TradeData.h, LiveOrderBook.cpp
```

## Design decisions

- **No cache in the engine** — Cache was removed so the engine stays a stateless processor. Consumers (GUI, server) implement their own caching. Same engine can drive both.
- **Queued consumer handoff** — Keeps GUI objects out of core; owners queue DTO callbacks to the appropriate Qt thread.
- **Strand instead of mutex in transport** — The strand serializes async operations on the `io_context` without blocking the I/O thread; a mutex would introduce blocking and complicate the async model.

---

## Transport security (TLS / WSS)

The internal stream server (`SentinelStreamServer`) defaults to `ws://127.0.0.1:8080`. Without TLS, all traffic (market data, server config, and eventually trade commands) is plaintext. TLS upgrades to `wss://` so the stream is encrypted and the client can verify the server.

**Encrypted:** `server_config`, `heatmap_slice`, l2 snapshots/updates, `market_trades`, candle and footprint/TPO history, `trade_command`, `order_update`, `position_update`, and control messages (e.g. `subscribe`).

**Live order-book grid:** Each server `snapshot` carries `tick_size` for that product and
`book_status` (`ready`, `unavailable`, `metadata_unavailable`, `invalid_tick`,
`invalid_snapshot`, `aggregation_unavailable`, or `invalidated`).
`l2update` carries the same `tick_size`; its prices are bucket prices on that grid. Both messages
carry a per-product monotonic `book_version` advanced on book resets and changed batches. A client
rebuilds its replica only from a two-sided `ready` snapshot with a finite positive tick, and drops
older versions or updates whose tick differs from that snapshot. An unavailable snapshot clears a previously ready
replica. New subscribers receive the current aggregated book and tick; existing subscribers receive
a fresh snapshot whenever an upstream snapshot resets the server grid, before subsequent deltas.
During rollout, a new GUI connected to an older server that omits `tick_size` keeps the live book
unavailable until the server is upgraded; it does not guess a tick from price or global config.
For non-BTC products the server receives exact Coinbase quote-increment metadata from the journal header,
then uses the roller near-grid rule on the current raw BBO midpoint (~1 bp nearest 1-2-5, rounded up to
a quote-increment multiple). Missing or invalid metadata leaves the aggregated live book unavailable;
the server retains the accepted upstream snapshot and every in-band native-price delta meanwhile.
When metadata arrives it builds an authoritative snapshot from that current raw state without a
metadata-driven upstream reconnect or recorder invalidation. A real upstream invalidation clears
the raw state until the journal worker re-seeds a valid snapshot. BTC-USD keeps the configured
`orderbook.tick_size`. Metadata never comes from a server REST request. Raw recorder
snapshots and deltas remain independent of this live aggregation gate. For every product, the server
retains raw price levels only inside the live book's band, sums all quantities in each tick bucket,
and adjusts the bucket total when one raw level changes or is deleted. The wire carries these bucket
totals; the GUI replica applies them as absolute quantities.

**Handshake:** TCP connect → TLS 1.3 ClientHello/ServerHello (server uses self-signed EC cert with SANs for localhost/127.0.0.1) → TLS Finished → HTTP WebSocket upgrade → JSON subscribe/server_config/heatmap_slice.

**Certificates:** Generate with `certs/gen-certs.ps1` (Windows) or `certs/gen-certs.sh` (Linux/macOS). Outputs `certs/sentinel-server.crt` and `certs/sentinel-server.key` (gitignored). Configure in `server_config.yaml` under `tls.cert_file` / `tls.key_file` and in `client_config.yaml` under `client.server.ca_file`. If `ca_file` is missing or invalid, the client falls back to `verify_none` with a log warning (acceptable for local dev only).

**Future:** Client authentication (e.g. HMAC-SHA256 of server nonce) can gate `trade_command` after the WSS handshake; the existing `Authenticator` in `libs/core/marketdata/auth/` can be used for signing.

---

## Trading stream (paper mode)

The stream protocol includes a paper-trading vertical:

- **Client → Server:** `trade_command` — `PLACE_ORDER`, `CANCEL_ORDER`, `CANCEL_ALL`, `FLATTEN`.
- **Server → Client:** `order_update` (order lifecycle), `position_update` (position and unrealized PnL).

`algo_command` (`start`/`stop`) is also accepted, but the server only logs success,
refusal, or exceptions. It sends no correlated command acknowledgement, rejection,
stop event, or current running-algo snapshot. `algo_order_event` reports an order
emitted by an algorithm and can prove activity; an order rejection is **not** a
start-command rejection. Silence cannot confirm either a successful start or stop.
The paper ticket therefore labels these commands pending/unconfirmed, and shows
running only after observing an algo order event. `trade_command` carries a
`command_id`, but `order_update` does not echo it, so manual command acceptance
cannot be correlated reliably (and cancel/flatten may emit no order update).
The stream also does not publish base/quote increments; ticket precision preserves
eight decimal places, rounds submitted limits to the editor's displayed decimals,
and displays small prices without truncation. A symbol switch clears the ticket's
limit until a new trade initializes it or the user enters a price. The server
remains responsible for validating executable increments.

The server runs paper execution (no real broker); fills use last trade price plus optional `trading.slippage_bps` from `server_config.yaml`. For setup and usage, see **`docs/PAPER_TRADING_QUICKSTART.md`**.

---

## Related documentation

- **`docs/ARCHITECTURE.md`** — System overview, client–server pipeline, rendering.
- **`docs/PAPER_TRADING_QUICKSTART.md`** — Paper trading configuration and hotkeys.
- **`docs/CONFIG.md`** — Server and client YAML options.

## Recording v2 core (HMC2)

`recording::BookRecorder` and `Hmc2Store` live in `libs/core/servermodel` and do not depend on GUI Qt. The recorder takes full, unclipped absolute L2 batches from one producer, and performs integration, compression and disk I/O on its own worker. `onInvalid` breaks continuity and ends observation; only an accepted snapshot restores that continuity. With continuity intact, updates keep reconstructing empty, one-sided or crossed books, but these intervals are unobserved: `observedMs`, row integrals and peaks do not advance, and the minute has the existing partial flag. Observation resumes on the update that restores both sides, best bid strictly below best ask, and a representable midpoint. Observation intervals are half-open: the suspending update's sizes are excluded from peaks, and the resuming update's sizes are included. A continuous book that remains empty, one-sided or crossed for `oneSidedSuspectMs` (30 s on the integration clock) raises `snapshotWait` with reason `unobserved suspect`; a validated refill or accepted snapshot clears it. Sequence gaps, malformed L2, row overflow, journal boundaries/gaps and transport events still invalidate continuity and require snapshot recovery. There is no five-second one-sided invalidation or new recording flag. An invalid interval preserves the minute's earlier numerator and peaks. Zero-observation minutes are omitted. The roller supplies envelope and capture-receive time explicitly through `onSnapshotAt` / `onUpdatesAt`; `drain()` fences deterministic replay. The model owns no recorder and never requests capture resnapshots; the serving roller does (see Server integration boundary).

Each side has an ordered, pooled price map (O(log levels) updates, O(1) best bid/ask access). Each layer has a pooled hash map keyed by `(integer row, side)`, with lazily advanced integrals and a reused touched-row buffer. One message is applied in full before its final row sizes update peaks. Malformed prices/sizes invalidate the whole batch's book continuity. Prices use checked nearest rounding in integer price units; an inclusion window tests the row's lower price edge, so bounds are `ceil(minMid * lowFrac * priceScale / rowTickUnits)` through `floor(maxMid * highMult * priceScale / rowTickUnits)`, inclusive on both sides. Empty entries inside minute bounds mean observed zero, not a missing book.

Enqueue carries the journal's capture-receive timestamp; idle ticks use the last actual `local - envelope` offset. Envelope time is clamped to a nondecreasing integration clock. A crossed minute is sealed before applying the new batch, then held until `clock >= bucketEnd + latenessMs`. Backward messages cannot revise sealed integrals, even within the lateness hold; older-than-committed messages also set the current minute's late flag. Lateness is therefore a commit delay, not an event-reordering buffer. Shutdown drains accepted messages but does not advance time: the open minute and any sealed records still in the lateness hold are dropped. Crash has the same uncommitted-tail loss. Restart begins observation at the new snapshot, never the last stored timestamp. Upstream must deliver disconnect/gap/suspend invalidations in order before timer advancement; silence alone is not evidence of an invalid book.

The queue bounds queued level count and uses a 4,096-slot ring with at most 4,095 admitted messages. Admission blocks until space is available; an oversized atomic snapshot is admitted alone. Stop wakes blocked producers with an error, and owners join them before destruction. There is no queue-drop invalidation path. Symbols are interned once on the producer, so even long symbol names do not allocate on repeated enqueues. Pooled containers and buffers reuse their high-water storage; new symbols, larger books/batches, minute sealing and hourly work can still allocate.

### HMC2 byte layout

All scalar fields are little-endian, with no native padding. IEEE-754 `f64` is used for floating fields. Symbols/layers are 1-255 ASCII letters, digits, dot, dash or underscore, excluding `.` and `..`.

Path: `<root>/<symbol>/<layer>-<tfMs>/<YYYY-MM-DD>.hmc2`, selected using the **bucket start UTC day**. A configuration or file-schema mismatch against the latest generation creates `<YYYY-MM-DD>.g<N>.hmc2`, starting at N=1. Returning to an old configuration also creates a new generation. Readers visit generations numerically and return the last valid record per bucket, sorted chronologically. Queries use `[startMs, endMs)` bucket starts. Supported UTC years are 2000 through 2200 inclusive (`[2000-01-01, 2201-01-01)`). Appends and path construction reject times outside this range before calendar conversion; range queries clamp their bounds. The recorder also rejects event/local clock values outside it, preventing microsecond timestamps from advancing the minute loop.

File header: new minute writes use schema 3 (temporal deltas); new hour writes use schema 4 (absolute with coverage). Readers also accept schemas 1/2 as absolute records and schema 3 hours as approximate coverage:

| Field | Encoding |
| --- | --- |
| Magic | u32 `0x32434d48` (bytes `HMC2`) |
| Schema | u16 `3` (minutes), `4` (hours) |
| Header length | u32, includes the 14-byte fixed prefix; maximum 65,536 |
| Header CRC | u32 IEEE CRC-32 over the **whole header with this field zeroed** |
| Symbol, layer | Each u16 byte length followed by bytes |
| Timeframe | i64 ms; 60,000 or 3,600,000 |
| Price scale, row tick | f64 price units per quote currency unit; i64 price units |
| Size floor, codes/octave | f64, f64 |
| Config hash | u64 |

The recorder uses FNV-1a-64 over the serialized header body (config hash zeroed), then f64 lowFrac and highMult. Header fields are compared as well as the hash. Hour records retain the originating minute policy hash, allowing rebuilds to exclude incompatible generations.

Record framing: u32 magic `0x32524348` (bytes `HCR2`), u32 compressed length, u32 raw length, u32 IEEE CRC-32 of **compressed bytes**, followed by one zstd frame (compression level 3). Raw length is bounded to 16 MiB; compressed input is bounded to `ZSTD_compressBound(16 MiB)` before allocation. Payload:

- i64 bucketStartMs; u32 observedMs; u32 flags.
- i64 bidRowLo, bidRowHi, askRowLo, askRowHi (inclusive; lo > hi is empty).
- f64 midOpen, midClose, midMin, midMax; u32 entryCount.
- Schema 1/2: sorted unique `(row, side)` entries: LEB128 zigzag i64 row delta (first relative to 0), u16 TWAP magnitude with ask in bit 15, u16 peak magnitude. Every record is absolute.
- Schema 3: after entryCount, u8 kind (`0` keyframe, `1` delta), then i64 predecessor bucketStartMs (`0` for keyframes). EntryCount counts changes for a delta, including removals.
- Schema 3 entries: LEB128 zigzag i64 row delta (first relative to 0), u8 side (`0` bid, `1` ask), LEB128 zigzag `(twapCode - previousTwapCode)`, LEB128 zigzag `(peakCode - twapCode)`. Previous TWAP is zero for keyframes/new keys. Repeated row with bid then ask uses row delta 0. Codes remain 15-bit magnitudes; side is separate.
- Schema 4 is restricted to tf=3,600,000 and kind=0. It uses the schema 3 entry layout followed by u32 `coveredMs` on **each entry**. After all entries: u32 coverage-run count, then each run as i64 lo, i64 hi (inclusive native rows), u8 side, u32 coveredMs. Runs are sorted by `(side, lo)`, disjoint within a side, and include covered zero rows. Each duration is positive and at most record observedMs; every entry must lie in a same-side run with the same duration. Coverage outside all runs is zero. Raw/frame length caps still apply. Schema 1-3 hour entries receive record observedMs as their in-memory coveredMs and flag bit 4 (approximate coverage).
- Keyframes list every key. Deltas list only changed keys; omitted keys retain both codes. A removal reconstructs TWAP and peak as zero. A record containing a zero-TWAP entry uses a keyframe: this preserves legitimate instantaneous peaks with zero integrated TWAP without confusing them with delta removals. Empty books have no entries. Bounds, mids, observation time, and observation flags are absolute metadata on every record; changing bounds does not implicitly remove keys.

A keyframe starts every UTC 15-minute interval containing a record, and is also required on writer reopen/eviction, after rollback/write failure, and on a gap, duplicate, or backward timestamp. Hourly records are therefore always keyframes. Delta predecessors must be the immediately preceding record in that same file and series, exactly one timeframe earlier and inside the same 15-minute interval. Writer state advances only after a durable append; the cache holds at most 64 file writers and eviction discards the base. Sparse changes with an unrepresentable signed row jump, or replacements exceeding the raw limit, fall back to a keyframe.

Range readers reconstruct from the query start's UTC 15-minute boundary (at most 15 minutes of preceding records), maintaining an independent base per file before deduplicating generations. The format has no seek index: readers still scan framing and compressed payloads to find timestamps, but do not reconstruct older entry states. A missing/corrupt predecessor invalidates the chain: deltas warn and are skipped until a valid keyframe, never applied to another generation or an earlier surviving bucket. Framing, CRC, zstd, varints, code ranges, sorted unique keys and reconstructed state size remain bounded/validated. Readers never repair.

Flags: bit 0 partial (`observedMs < timeframe`), bit 1 resynced (snapshot occurred), bit 2 late events, bit 3 underflow (a positive stored TWAP/peak was below the size floor), bit 4 approximate coverage (legacy hourly records). Positive underflows encode as magnitude 1. Readers return side separately from both magnitudes. Extreme representable sizes saturate the fixed size code.

One root `.lock` is held for the writer's lifetime. Every append flushes the stream and synchronizes the file (macOS F_FULLFSYNC with fsync fallback). Every directory and its parent chain are synchronized on first writer use, including existing directories left by a previous process; directories receiving a new file are synchronized again. New generations use exclusive creation so a filename collision cannot overwrite history. Windows attempts a native directory metadata flush and surfaces a failure if the filesystem denies it; this path needs platform validation. An append records its starting file offset. On failure it attempts to truncate back to that offset and synchronize the rollback, evicts the cached writer, and rethrows the original error. The next append reopens and repairs the file instead of losing the remainder of the day; rollback failures are separately logged. Disk failures are logged and counted; failed minute writes never enter the live hourly accumulator.

Locked writer reopen repairs only an incomplete terminal frame at a known record boundary. Complete bad-CRC frames, bad magic and interior corruption are retained and logged. Recovery scans forward in chunks and accepts a resync candidate only after framing, CRC, zstd and payload validation; a length extending past EOF does not cause truncation if a later valid record exists. A possible torn tail following interior damage is retained, with an explicit warning that its boundary is unverified. Read-only queries never truncate. Bad headers are skipped, and a writer creates a fresh generation without overwriting empty or partial header files. Range reads warn and skip unreadable files or directory entries, preserving readable history from other days; write I/O errors still throw.

Hourly layers rebuild the current hour from deduplicated, committed minute records of the matching policy on the first snapshot. Startup also reconstructs and writes the immediately preceding hour if its matching-policy rollup is missing, closing the crash window between the last minute commit and the hourly write. Existing hourly records are left alone. Recovery/write failures are logged without invalidating unrelated live books, and hourly accumulation failures do not label an already-committed minute as lost. For each row/side, the mean is `sum(decodedMinuteTwap * minuteObservedMs) / sum(minuteObservedMs where minute bounds cover row)`; a covered absent row contributes zero, and an uncovered row contributes neither numerator nor denominator. Peaks use decoded maxima. The hour is persisted after its own lateness watermark, including when the book is invalid at hour end.

Schema 4 removes the earlier hour-coverage limitation: sparse quantities carry their own denominator, and coverage runs preserve the denominator for absent zero rows and holes in moving bounds. Multi-hour means remain exact with respect to stored, quantized TWAPs; the hourly mean still incurs the fixed log codec's normal rounding error.

### Recording reader and test projection oracle

`recording::Hmc2Reader` is a worker-owned read session. `visit` reconstructs records in chronological order for `[start,end)`, using the last valid record per bucket across numeric generations and append order. It caches frame metadata for at most 64 files, the active reconstructed record, and a replay window capped at 16 records / 262,144 entries. It never retains the requested range's records. The original tolerant `Hmc2Store::readRange` remains available for recorder recovery.

A shared `ReadControl` bounds source records, entry work, wall time and cancellation across availability and scans. Cold index discovery charges source records without parsing entries; a budget stop retains the completed metadata prefix and resumes on the next request. Appends extend that index from its prior boundary only after validating the old tail frame's framing/CRC and header. Shrink, same-size rewrite with changed mtime, or a damaged anchor rebuilds it. Index and directory-listing caches use LRU eviction (64 entries); directory mtime refreshes the listing, and file size/mtime refreshes series availability. All public reader operations and destruction assert the constructing thread in debug builds; S2 needs one reader per history worker.

Limits are soft admission budgets, clamped to at least 1 source record, 1 entry-work unit, and 10 ms. Reconstruction charges inherited plus changed entries and a conservative coverage-run bound; cache hits still charge. The first selected record and its at-most-15-record delta chain are admitted as one unit even when they exceed the requested record/entry budget. Availability likewise admits its first edge candidate on each side, so discovering the floor cannot repeatedly consume the entire admission budget. A first output column with at most 16 native time buckets defers the deadline through projection; larger rollups must fit their budget. Counters report actual work, including overshoot. Cancellation remains immediate at cooperative checks, including inside these first units. Discovery may still return no columns, but a reused reader advances its cached cursor. One-shot root calls discard that progress, so history workers must reuse their reader. Default `ReadLimits` is unlimited.

I/O policy: discovery warns and skips permission-denied/corrupt files, continuing to other days/generations like `readRange`. A discovery that skipped a file is not cached as definitive availability, so recovery is retried. Other I/O failures while discovering a required range or availability boundary return IoError; they cannot establish an empty series. Once a candidate is selected, open/read failure, disappearance, shrink or bucket-identity change returns `IoError`; it does not advance the scanned interval or cache a gap. CRC/zstd/payload corruption is distinct: warn, skip the bad candidate, and try an earlier valid generation. Directory enumeration failures remain errors, never proven empty history.

`recording::buildPage` and `LiveBuilder` reside in `tests/support/servermodel`, linked only by the `sentinel_recording_oracle` test target. Production uses sparse chunks and raw live tails. The test oracle `buildPage(reader-or-root, BuildRequest, StopToken)` returns ascending columns and a common `(lo,tick,rows)` band, layer and size-code scale. Requests accept 1m multiples below an hour, integer-hour multiples thereafter, at most 1024 output buckets, up to 16384 rows, and an optional exact display tick. Automatic ticks use the shared `recording::ladderTickUnits` ladder (`{1,2,2.5,5} x 10^k`, restricted to native-tick multiples) until `ceil(hi/tick)-floor(lo/tick) <= rows`; the returned row count is that actual count. Explicit ticks must fit the band and be integral native-tick multiples. Cells are half-open on the absolute price grid; arrays descend from the highest row, with lower-edge prices. Minute history selects near below the recorded deep tick and deep otherwise. Hour history uses deep; near-resolution hour requests are rejected because the recorder does not persist near hours.

Rollups preserve numerator and denominator per native row and side before summing at the display tick. Covered absent entries contribute zero; uncovered rows contribute neither numerator nor denominator. Both sides remain separate until dominant-side projection, with bids winning ties. Columns carry absolute log cells, linear u16 dominant-side quantities with `quantityScale`, observedMs, flags, and an LSB-first validity mask in the same descending order. A row is valid only when **every native row on both sides** was covered for all observed time on its source grid. Partial observation sets `partial` without turning observed zeros into unknown rows. Legacy hours propagate `approximate coverage`. Grid changes across recording generations are evaluated per output bucket. Compatible grids retain their own native-row denominators; their display-row side quantities are combined by observed duration before choosing the dominant side, and validity requires full coverage in every grid. If any constituent native tick does not divide the display tick (for example old $10 rows at a $25 display tick), that output column carries zero cells/quantities and an all-unknown validity mask, retaining its observed duration and flags; the page still completes. A $50 display tick represents both old $10 and new $5 rows. No native row is split to invent finer detail.

For tf >= 1h, persisted hours are combined with minutes from the latest unpersisted hour only. Minutes belonging to a persisted hour never participate. Result availability uses output bucket starts. `endMs=0` selects latest; nonzero end is inclusive and aligned down to output timeframe. The builder walks output buckets backward, then returns columns ascending. Only completed output buckets enter `[scannedStartMs,scannedEndMs)`; unfinished rollups are discarded on budget/cancellation. Gaps inside that interval have been scanned. `nextEnd` is the preceding output bucket; `exhausted` means the availability floor was reached, never merely that a page was short. An I/O failure returns a status without claiming the unread interval. Root overloads are one-shot; serving workers should reuse a reader.

### Sparse recording chunks (S2 codec, served by S3)

**Levels and spans.** Every chunk source stores three levels: 1m (recorded), and 1h and 1d (rollups built from those minutes, below). Chunk spans: 1m -> one UTC hour, 1h -> one UTC day (24 columns), 1d -> `kDayChunkDays` (16) UTC days. Chunk starts are whole spans after 2000-01-01 UTC, the first supported day (`heatmap::chunkStartOf`; for 1m and 1h this equals Unix-epoch alignment). The 1d span is fixed so key arithmetic stays a modulo; 16 days keeps a 1d chunk's raw payload near 4 MB on BTC-USD (about 0.2-0.3 MB a day), a quarter of the 16 MiB payload limit, so a widest 1d view (1500 days) needs about 94 chunks instead of one per day.

`recording::buildChunk` reads one minute chunk (one UTC hour) of a chunk source into `heatmap::SparseColumns`; it refuses a rollup level, which `recording::RollupStore` serves. The recorder's own `deep-3600000` hour files are not a chunk source. The chunk start is aligned to the span. The output extent is the full span, but `scannedRanges` proves only the reader's completed scan. For an open chunk, pass `BookRecorder::watermarks(symbol,layer)`; the builder scans only complete minutes before the minute cutoff, which has already applied recorder lateness. `chunkState` seals when the cutoff reaches the chunk end; a sealed state reports `committedThroughMs` equal to the chunk end and revision 0, so sealed bytes and content hashes do not depend on how far the recorder has moved on. Sealed frames require one complete scanned range. The shared, mutex-protected encoded LRU stores sealed chunks only, is bounded by encoded bytes, and returns immutable shared buffers.

`heatmap::ChunkCodec` v3 encodes a chunk as little-endian `SHC1`, `u16` wire version, `u8` kind, `u8` sealed state, length-prefixed symbol and source id, signed `i64` level/start/end, grid identity (`u64` config hash, `i64` row tick units, `f64` price scale), `SizeScale` (two `f64`), signed `i64` committed-through time, `u64` revision, `u64` FNV-1a content hash of the header prefix and uncompressed payload, `u32` column and entry counts, `u32` raw and compressed lengths, then a zstd frame. Request identity is outside cached SHC1 bytes: `SHE1`, `u16` envelope version, `u64` request id, then SHC1 bytes. The header grid and scale are zero when the chunk contains multiple native identities; each native constituent always carries its own identity and scale in the payload. The payload starts with aligned scanned ranges, then column/native records (bucket, observed duration, flags, coverage runs, entry counts), then entries in column/native order. Each native record carries a `u8` flags byte (bit 0 composed, bit 1 numerator low parts) and a `u8` covered-duration sidecar flag. Entries use row-delta unsigned varints with the side in bit 0, `u16` original size codes, optional covered-duration varints (native columns only), and for a composed constituent an `f64` numerator per entry (decoded mean times covered duration, as composed in memory) plus, with flag bit 1, the `f64` remainder of a wider `long double`, so numerators round-trip exactly on every platform (the bit stays clear where `long double` is a `double`). A composed constituent's per-entry durations come from its coverage runs. Composed constituents are allowed only in rollup levels (never 1m or live columns). Binning or recomposing them uses the exact numerators, never a second quantization of the rounded codes. v3 added the 1h/1d levels for every source and composed numerators; v2 and v1 frames are refused (no compatibility shim). Raw payloads and whole response envelopes are limited to 16 MiB. The decoder checks the zstd frame content size before allocating, then rejects unknown versions and malformed lengths, counts, hashes, or sparse invariants. Both encoder and decoder validate source/level and timestamp bounds/alignment before deriving end times, including `not_modified` control frames.

**Chunk sources.** The key is `(symbol, source, levelMs, startMs)`. `source` is a neutral id; the wire never names near/deep. Today's ids are `hmc2.near` and `hmc2.deep`, each with the 1m, 1h and 1d levels. They are migration-only: they map to the HMC2 recording layers while HMC2 is the source (`heatmap::kChunkSources`), and nothing may branch product or renderer behaviour on them. Raw-derived levels will add new ids. `SparseColumns::layer` still holds the HMC2 layer name in memory only.

**Rollup levels (`recording::RollupStore`).** The server owns a derived, rebuildable store of the 1h and 1d levels for both layers, in its own directory beside the HMC2 tree (`recording.rollup_dir`, default `<roller_shadow.dir>-rollups`; deleting it is safe). Both paths are resolved (absolute, symlinks followed) and must be disjoint in both directions: the store is refused when it is the tree, inside it, or an ancestor of it, or when a path cannot be resolved; the store never writes through a link below its root. Reference picture (owner decision 2026-11-06): minutes composed with the production compose. Each closed UTC day of each (symbol, layer) is composed from its 24 minute chunks with `heatmap::compose` (`ComposeOptions` clip to the day, exactly what a view does with the same minute chunks) at 1h and at 1d, and written as `<root>/<symbol>/<layer>/<levelMs>/<YYYY-MM-DD>.shr`: a 24-byte header (`SHR1`, `u16` version 1, `u16` 0, `u64` minute-file fingerprint, `u64` frame length) and one SHC1 frame.  The 1h file is the sealed day chunk itself; the 1d file is the day's piece of its 16-day chunk (an open frame that scans only that day). Writes are atomic: an exclusively created temp file `<YYYY-MM-DD>.shr.tmp-<pid>-<n>`, fsync, rename, then fsync of the directory. Stale temp files are never served; when the builder starts it removes regular files with exactly that name pattern in its own `<symbol>/<layer>/<levelMs>` directories (no recursion, no links followed, errors logged and ignored).
- **Builder:** one dedicated thread at background priority (macOS QoS background, Linux nice 19, Windows background mode), never the recorder or network threads, started and stopped with the stream server. A pass runs every minute (or at once after a corrupt file is found): for each roller product and layer it walks closed days newest first, from the day before the minute watermark's UTC day down to the first recorded minute's day, and builds every day whose files are missing, torn, marked corrupt (a mark stays until a replacement is written), or whose fingerprint (names, sizes and modification times of that day's minute files) no longer matches. A day is closed when the recorder's minute cutoff (`ChunkService::effectiveWatermarks`) has passed its end; the open day is never built. A day without minute files is a recorder gap and needs no files. Builds are idempotent and deterministic (a rebuild writes identical bytes). Measured on BTC-USD (2026-09-30..11-05): near 165 ms and deep 400 ms per day (median), about 1.0-1.5 MB per day per layer for 1h and 0.12-0.19 MB for 1d, peak RSS 0.43 GB while composing a deep day.
- **Advertised interval:** per (symbol, layer) the store indexes the contiguous run of built days ending at the newest built day, `[runStart, runEnd)`. 1h is advertised as `[runStart, runEnd)`, or from the first recorded hour once the run reaches the first recorded day; 1d likewise from the first recorded day once complete, but while a backfill is in progress only from the first 1d chunk boundary inside the run, so a client never caches a 1d chunk that later gains days without its cutoff moving. `committed_through_ms` of both is `runEnd`. Every advertised change bumps a generation that is part of the availability fingerprint.
- **Changed content:** sealed 1h/1d chunks behave like sealed 1m chunks: revision 0, and a GUI does not automatically revalidate the sealed chunks it holds; a GUI restart guarantees a fresh fetch (its chunk cache is in memory only), though cache eviction and refetch, or a hash-mismatch response, can replace a body earlier. When a closed day's minute files change, the next pass rebuilds that day (fingerprint mismatch), so every later fetch gets the rebuilt picture. The fingerprint sees file names, sizes and modification times only. **Contract for tools that rewrite closed-day minute files** (for example a roller backfill): delete that day's rollup files (`<rollup_dir>/<symbol>/<layer>/{3600000,86400000}/<YYYY-MM-DD>.shr`) or the whole rollup directory after the rewrite; the server then rebuilds them. A rewrite that keeps names, sizes and times and skips this step is not detected. The roller does not do this yet: docs/TODO.md, "Roller: delete rollup .shr files after rewriting closed-day minutes".
- **Serving:** a 1h key serves its day file after a full frame check (hash and structure), cached by file size and modification time (128 MiB LRU). A 1d key merges its day pieces over `[max(chunk start, runStart), min(chunk end, runEnd))` (days before the first recorded day are scanned gaps once the run is complete); it is sealed when that covers the whole chunk, otherwise open with `committedThroughMs = runEnd`. Its revision is 0 when sealed, otherwise 1 + scanned days. A missing or corrupt file of a day that has minutes is served by composing that day's minutes on the request worker (same bytes, slower), logged, counted (`sentinel_rollup_fallback_serves_total`) and queued for rebuild. A 1h key for a day that is not closed is `unavailable`.
- **Scope:** the recorder still writes `deep-3600000` hour files; they are no longer read for serving. Deep 1h+ views are therefore minute-built: about 0.7% of deep cells at 1h and coarser differ by one colour code from the recorder-hour picture (a second quantization in the recorder rollup).

**Control frames.** Kind `not_modified` (3): `SHC1`, version, kind, `u8` sealed, symbol, source, `i64` level and start, `i64` committed-through, `u64` revision, `u64` content hash of the chunk the requester already holds; no payload. Kind `error` (4): `SHC1`, version, kind, symbol, source, `i64` level and start (echoed as sent, not validated), `u16` code (1 `invalid_request`, 2 `unavailable`, 3 `busy`, 4 `build_failed`), length-prefixed message (at most 255 bytes). Both are exact-length. Kind `live_column` (2) carries the native raw tail described below.

### Heatmap chunk wire (S3)

Requests and availability are JSON text frames; chunk replies are binary WebSocket frames (`SHE1` envelope around `SHC1`). Existing JSON messages are unchanged. The server sets the frame type per queued write (`PendingWrite::binary`); the client branches on `got_binary()`. A binary frame sent by a client is logged and dropped by the server.

- **Request (client -> server):** `{"type":"heatmap_chunk_request","req":<u64>,"symbol":..,"source":"hmc2.deep","level_ms":60000,"starts":[<i64>...],"have_hash":["<16 hex>"|""...]}`. 1..64 starts; `have_hash` is absent or parallel to `starts` (hashes travel as 16 lowercase hex digits because JSON numbers are not reliably 64-bit). Each start gets exactly one binary reply carrying `req` in the `SHE1` envelope: `chunk`, `not_modified` (when `have_hash` equals the current content hash), or `error`. A malformed request gets one `invalid_request` error with the echoed `req` (0 when unreadable) and start 0.
- **Sealed and open:** sealed 1m chunks never change and are served from the shared byte-bounded LRU (256 MiB) as exact bytes; sealed 1h/1d chunks are cached by the rollup store and, like 1m, never change for a running GUI (see "Changed content" above). Open 1m chunks are rebuilt per request and carry `committedThroughMs` plus a revision: `1 + complete level buckets before the cutoff`. Content is a function of that cutoff, so a later revision is never older data. The client keeps the newest state per key (sealed beats open; open `(revision, committedThroughMs)` never goes back) and reports an older reply as a local `superseded` error, because two server workers can finish the same key out of order.
- **Cutoffs:** 1m uses the recorder's minute watermark when it records the series in this process; otherwise, with t = now - 5 min (a freshly started series publishes its first watermark within about a minute), minutes before t's UTC minute are committed. 1h and 1d use the rollup store's built interval.
- **Availability (server -> client):** `{"type":"heatmap_availability","symbol":..,"chunk_wire_version":3,"sources":{"hmc2.deep":{"migration_only":true,"oldest_ms":..,"latest_ms":..,"grids":[{"config_hash":"<hex>","row_tick_units":..,"price_scale":..,"size_floor":..,"codes_per_octave":..}],"levels":[{"level_ms":60000,"chunk_span_ms":3600000,"oldest_ms":..,"latest_ms":..,"committed_through_ms":..}]}}}`. `oldest_ms`/`latest_ms` are persisted bucket starts; `grids` holds the newest 1m record's native grid. A source is listed when it has recorded minutes; its levels are 1m, then 1h and 1d where the rollup store advertises them. The client reads each level only inside its `[oldest_ms, committed_through_ms)` (`tiles::chunksFor`: the coarsest level dividing the timeframe, finer levels at the edges, minutes for the open tail). Sent on every `subscribe` (when recording is available) and pushed again when a minute cutoff or the rollup generation changes (checked once a second, built on a history worker). The client reads `chunk_wire_version` before any other field and refuses a message whose version differs from its own (a v2 message lacks the v3 fields), signalling the mismatch that clears the fetcher's cache and stops fetching. `server_config.recording.chunk_wire_version` announces the version too.
- **Budgets:** per session at most 4 chunk jobs building (half of the server-wide eight-job history pool, so one session cannot starve candle/TPO history) and 32 MiB of chunk replies queued; a start beyond either gets an explicit `busy` error, never a silent drop. A full server pool is also `busy`. Chunk replies are accounted separately from the 16 MiB slow-client write limit; immediate refusals use the ordinary limit. One admitted reply can overshoot the byte budget by at most one frame (16 MiB).
- **Client (`SentinelStreamClient`):** `requestHeatmapChunks(symbol, source, levelMs, starts, haveHash)` returns the `req`. Binary frames are decoded on a dedicated one-thread pool (FIFO, off the network thread), with at most 256 queued/running frames and a 64 MiB decode backlog (both limits include work from invalidated connections until it retires); overflow is refused as `client_overloaded`. Frames shorter than the 14-byte envelope header are refused inline as `malformed`, without posting decode work. Admission refusals are coalesced to one notification per reason until a decode completes (or a new connection starts), so rejection cannot itself enqueue an unbounded error backlog. Signals: `heatmapChunkReceived(req, shared const ChunkFrame)` for `chunk` and `not_modified`, `heatmapChunkFailed(error)` for server errors and the local codes `malformed` (the decoder rejects hostile input before allocating), `superseded` and `client_overloaded`, and `heatmapAvailabilityReceived`. Disconnect invalidates outstanding binary decodes immediately. The connection epoch is checked after decoding under the same lock that clears ordering state and publishes success/error signals, so even a decode already running cannot publish or repopulate ordering state across disconnect/reconnect. Consumers connect these signals with `Qt::QueuedConnection`. No client-side request budget yet: the S5 controller paces requests.
- **Teardown:** jobs hold only a weak session reference; a reply for a closed or destroyed session is discarded, and `stop()` joins running builds.

### Raw heatmap live tail (S5L-a)

Servers advertise `server_config.recording.chunk_live: true` only when recording is available and the live service exists. A client sends
`{"type":"heatmap_live_subscribe","sub":<u64>,"symbol":..,"sources":["hmc2.deep","hmc2.near"],"since_ms":<i64>}`
and cancels with `{"type":"heatmap_live_unsubscribe","symbol":..}`. Sources must be distinct supported ids. A valid re-subscribe replaces the subscription for that symbol (including its sources, id, and final markers); malformed requests leave it intact. A session holds at most eight raw-tail symbols. The live service has a separate cap of 128 symbol subscriptions, independent of the 64 legacy recording views. Subscribe refusals use a binary SHC1 `error` with SHE1 request id `sub`, an echoed symbol and empty source/start 0: `invalid_request`, `unavailable`, or `busy`. These subscriptions only observe series recorded by this process; they do not start an upstream recorder.

Each update is a separate SHE1/SHC1 kind `live_column` (2) frame per source. SHE1's request id is `sub`. Its SHC1 header has the same layout as kind 1 **with one additional signed i64 `tailStartMs` immediately after `endMs` and before the grid identity**; the content hash includes this field. The key uses level 60000 and the UTC hour containing the latest known live minute. `endMs` is that minute's exclusive end, not the hour end. The minute is inferred as the later of the committed cutoff and newest provisional start; before the next open publication at rollover, the newest provisional can still be the just-closed minute. The body is native 1m `SparseColumns` over `[tailStartMs,endMs)`, allowed to reach into at most the preceding hour-sized chunk. State is always open with the recorder's exclusive `committedThroughMs` (zero before any proven cutoff) and the nonzero `LiveCache` series revision. Cutoffs and extents are minute aligned and constrained to the supported recording timestamp range before arithmetic.

Within the byte budget described below, the body carries every retained provisional minute (up to 60 pending minutes plus the open minute), flagged `kProvisional`, and held committed finals whose start is at or after that subscriber's final marker. A final is before `committedThroughMs`; a provisional is at or after it. Column flags come directly from the recorder: an observed forming minute has `kPartial` and `observedMs` for time covered so far, while a complete lateness-held provisional can have `observedMs = 60000` without `kPartial`, including the newest column during rollover. A commit can precede the next open publication: its frame can contain finals without an observed open column. Each minute has one native grid constituent and the original size codes, with entries sorted by (row, side). `scannedRanges` is the coalesced union of transported minute records. It never claims that an omitted or evicted final was an empty recorder minute; such holes stay `NotLoaded` until a chunk scans them. Both encoder and decoder reject inconsistent flags, extents, states, scan claims, counts, grids, durations, or overflowing input. Existing SHC1 payload/envelope limits and hash/zstd validation also apply.

**Live rate.** The recorder publishes each layer's open minute at most once per `recording.live_publish_ms` of integration clock (default 500 ms, clamped to [250, 5000]; owner decision 2026-10-01), so live frames carry a new revision at 2 Hz. While a symbol's book is unobserved (empty, one-sided, crossed or invalid) nothing integrates, so the recorder publishes the open minute once and then only when an input changes (validity, observed time, flags, commit cutoff, minute); it never re-sends a byte-identical record. A failure while building an open-minute publication is logged (throttled) and skipped; it never invalidates a book. The live worker turns every ~100 ms and paces each subscription at half that interval (`liveCadenceMs`, 250 ms by default). It sends only a changed revision, so frames still follow publications (2 Hz). In steady state (provisional publications only, with no intervening final deliveries and no byte-budget backpressure) each publication goes out at the first worker turn after it; a refused admission backs the cadence off as described below. A cadence equal to the publish interval would, after turn rounding, fall behind the recorder, skip publications, and let the data age sawtooth through a whole interval.

The raw path uses `LiveService`'s existing worker and `LiveCache::snapshot()` only, with no disk reads and no recorder changes. It attempts changed revisions at the live cadence above. Entries, column storage, and compression scratch are reused; an encoding is shared across subscribers needing the same retained final suffix, and all subscribers with no pending finals share one encoding per (series, revision). Each session has an independent raw **1 MiB aggregate byte budget**, including SHE1 envelopes, separate from the chunk reply budget. Multiple raw frames can be admitted together, preserving the live rate for both sources and multiple symbols. Queued raw frames retain FIFO order behind the current write, ahead of queued history; bytes remain charged through write completion or discard. Refusal coalesces to the newest snapshot and doubles the cadence from its base up to 5 seconds (0.25 -> 0.5 -> 1 -> 2 -> 4 -> 5 s by default). Only accepted admission advances that source's final marker, so a refused final accompanies the next accepted frame while it remains retained. Successful admission restores the base cadence. After budget pressure, attempts rotate across sources to prevent starvation. Empty builds consume the revision without delivery or congestion backoff. Allocation/build failures are logged and retried with backoff. Replaced/unsubscribed queued frames are discarded; an already in-flight frame can finish and consumers must correlate `sub`.

Oversized tails produce a throttled warning naming symbol, source, and bytes. The builder removes oldest pending minutes until the envelope fits, preserving the newest known minute. If finals also need trimming, it removes their newest suffix so only an included final prefix advances the marker. Omitted minutes are excluded from scan claims and recovered through chunks; trimmed finals are not marked delivered. An indivisible oversized minute is logged and omitted rather than refused forever. The encoding cache holds at most 17 variants per series/revision (16 possible first finals plus the no-finals variant). SHE1 construction and the queue's owning string still make two per-subscriber copies; the byte budget bounds admitted wire bytes, not these transient copies.

On reconnect the client subscribes after `connected()` plus fresh availability, passing its stored open chunk's `committedThroughMs` as `since_ms` (zero is allowed for a cold subscription). Finals with starts at or after that value are resent if still held: the cache retains at most 16 finals, additionally bounded by entry count. Older data is fetched as chunks. Series revision increases within one server process; a server restart can reset it. Revision is not a persistent identity, and a fresh subscription/connection must accept the reset.

The client routes kind 2 to `heatmapLiveReceived(sub, shared const ChunkFrame)` without `acceptChunkOrder`; it uses exactly the S3 FIFO decode pool, frame/byte admission bounds, refusal coalescing, and connection-epoch invalidation. `subscribeHeatmapLive` returns a unique id from the chunk request id namespace, and `unsubscribeHeatmapLive` cancels a symbol. `ChunkTransport`/`SentinelStreamClientTransport` expose `subscribeLive`, `unsubscribeLive`, and `liveReceived`; the adapter maps subscription ids and rejects events from replaced subscriptions, sources, hosts, and disconnected epochs. The recording-only local transport returns id 0 for unsupported live subscriptions. The fake transport can script/hold/release live replies.

**Client handover (S5L-b):** `ChunkFetcher` owns one `LiveEdge` per (symbol, source) on the heatmap-data thread. `wantLive(chart, symbol)` / `releaseLive(chart)` share one live subscription per symbol over its advertised minute sources. A minute comes from the chunk if its start is less than that chunk's stored `committedThroughMs`, otherwise from live, never both. A missing final stays `NotLoaded`, never zero; a provisional minute omitted by a newer frame loses its previous value. Live data stays outside `ChunkStore`; stored cutoffs trim immutable buffered minutes, and only the latest 120 minutes of live records and missing-minute markers are retained. A frame ahead of a wanted, stored open chunk triggers immediate `have_hash` revalidation, including a follow-up if a commit advances while a request is in flight. Live frames preserve chunk retry deadlines and terminal failures. Refused live subscriptions retry with exponential 100 ms..5 s backoff. Charts keep their subscription interest for three seconds after panning away from the live bucket; symbol changes and chart destruction release it immediately. Reconnect waits for connection plus fresh availability, sends the minimum stored open cutoff across sources as `since_ms`, ignores old subscription ids, and accepts a revision reset in the new epoch. The previous picture remains frozen while disconnected.

The per-chart controller publishes `LiveSnapshot` separately through thread-safe `latestLive()` and queued `liveChanged()`. Sources contain composed columns plus their CPU GPU-upload image over `[startMs,openEndMs)`; all sources use the lowest L of the live-edge builds that may still draw. Normally L advances only on the node's upload report. A dropped or explicitly missing drawable span no longer holds L; prefetch and recent-timeframe slots do not hold it either. If no drawn span still covers the previous L, it can advance to the committed edge floor, leaving the abandoned interval loading. Lag behind the newest live bucket floor is capped at `max(tf, min(2 hours, 64 * tf))` (plus the forming bucket itself); the one-bucket minimum preserves 4h/1D rollover until the previous bucket's span uploads. The cap also applies to stalled cutoffs and slower sibling sources. Exceeding that cap moves L to the newest edge floor; a source wholly before L contributes no live image. Upload acknowledgements are retained only for the current live window. `SpanSourceBuild::completeEndMs` exposes E for the node's clip. `TimeComposer::ComposeOptions::forming` and the production live composer include a terminal proven prefix with its observed duration, so a commit before the next open publication does not blank a coarse forming bucket. Known missing provisional minutes and never-received finals before the wire cutoff still keep that bucket unloaded. The build pool caches committed contributions by chunk generation and unchanged pending contributions by immutable column identity; ordinary live updates recompose one bucket. `latestResolution()` merges history and live for Auto without replacing the historical SpanSet pointer, deduplicates by column/source (loaded history wins), and retains history when live resets. Consumers must use the client-local `LiveSnapshot::version` for upload identity (a server revision can restart or stay the same across a local chunk handover), and retain snapshots with held/fading pictures. A live frame composes on arrival after a 15 ms coalescing window, at most once per `liveMinIntervalMs` (default 500 ms, which composes every 2 Hz server frame; a frame after a short gap waits at most the shortfall); a measured composition median over 5 ms backs off to 5 seconds, recovering to the minimum interval after one at or below 5 ms. Cadence deadlines start at job admission. A consistent completed job may publish if its chart serial and live state still match, even when newer input arrived; newer input coalesces until the next deadline. Unrelated history events do not invalidate live work. Ordinary live frames refresh only the live window, and publications update its CPU ledger without reconciling the span plan. The `heatmap.live.compose` probe reports cost, recomposition counts and cadence.

### Process-wide chunk fetching (S5a)

`heatmap::ChunkFetcher` and its `ChunkTransport` live on the heatmap-data event-loop thread; controllers queue `want(chartId, keys, priority)` and `release` calls there. One fetcher and one `ChunkStore` serve every chart. Incoming transport signals and outgoing controller notifications use queued connections. The transport wraps the existing `SentinelStreamClient` API without changing its decode or ordering rules. The local lab transport builds native chunks on its own worker using a read-only `Hmc2Reader`; it never takes the recording writer lock. Requests use the last advertised availability snapshot. Polls publish only changed snapshots (each connection still gets a fresh initial message); scan failures retain the last good snapshot and log a throttled warning, without failing requests. Shutdown requests cooperative cancellation through the reader's `ReadControl` before joining the worker.

The fetcher deduplicates in-flight and current cached keys, selects work by priority, then batches by `(symbol, source, levelMs)` within the wire limit. It admits at most four chunks and 16 MiB of estimated work. A byte-blocked key stops admission of lower-ranked keys, preserving capacity for visible work. Cold chunks estimate 4 MiB each; warm chunks use decoded bytes, capped at one full 16 MiB wire frame (one large chunk can still progress). This admission estimate is not a bound on actual decoded size. The decoded store separately enforces its configurable byte budget (512 MiB by default) for `put`; externally held shared pointers are outside LRU accounting. The legacy blocking lab API retains its existing keep-newest budget exception until S5c. `busy`, local decode overload, and request deadline expiry retry with exponential 100 ms..5 s backoff, reset on disconnect or cache clear. Each request has a 30 s deadline from admission (configurable, using the fetcher's monotonic clock); partial replies do not extend it. Completion/expiry retires adapter bookkeeping through `forget(requestId)`, and late replies are ignored. A request-level sentinel key that matches none of its pending keys applies to all remaining keys in that request. An unreadable request id retries keys not explicitly identified as bad; identified terminal errors fan out normally, and `superseded` retires silently. A rejected store insertion retries with the same backoff for at most five attempts, then emits `chunkFailed` with `store_rejected`. Successful insertion or reconnect resets the attempt count.

A disconnect loses every request but keeps chart interests and cached bodies. The client adapter treats a second `connected()` as a down/up transition because explicit client disconnect does not emit `disconnected()`. Requests resume only after connection plus fresh availability for their symbol. Wanted open chunks revalidate with `have_hash`; sealed chunks remain current. The fetcher retains the hash's shared body until its reply, even if the store evicts that body meanwhile. `NotModified` preserves the generation; changed bodies and first sealing advance it and broadcast a revision to all charts. Availability advances revalidate only wanted open chunks of the affected source/level; an advance during a request schedules a follow-up if that response still predates the new cutoff. A wire-version mismatch or host replacement clears the store and rejects old outstanding replies. The adapter's `setClient` reports host replacement; attach it before connecting/subscribing the client so no connection or availability notification is missed. A current cache hit in `want()` emits nothing, and eviction is not signalled; controllers peek the store and call `want()` again after a miss. The fetcher marks every key some chart wants as wanted in the store, which never evicts it (even a body larger than the whole budget) until every chart releases it, so a delivered body is never refetched while it is wanted.

## Live trade rows and chart bubbles

Captured Coinbase `market_trades` -> roller `JournalFeed` -> queued
`ServerDataModel::onTrade` -> `tradeBroadcast` -> stream Session `on_trade` ->
Sentinel wire `type:"trade"` -> `SentinelStreamClient::tradeReceived` -> queued
`RemoteGridDataSource` -> queued `UnifiedGridRenderer::onTradeReceived`.
The existing live wire carries `product_id`, `price`, `size`, `side`, and RFC3339
`time`; it now also carries `trade_id` and `side_basis:"aggressor"`. These additive
fields do not require a server upgrade for bubbles: older servers already send time.
The client now preserves exchange time and ID instead of discarding them.
Invalid/missing time stays zero and cannot produce a bubble.

**Side contract:** `JournalFeed` converts Coinbase's maker `side` to
the aggressor side once at ingest, and trade frames carry `side_basis:"aggressor"` (see the
trade side section at the top of this file). The client uses `side` as-is when
`side_basis` is `"aggressor"`; an unmarked frame from an older server is treated as maker and
flipped (maker sell -> aggressor buy, maker buy -> aggressor sell); unknown side/basis stays
unknown. Server footprint delta is aggressor-based.

The GPU chart retains the newest 100,000 valid executions by event time in a fixed,
time-ordered ring for its selected symbol, including while Trades is off. Normal
append is O(1); late executions use ordered insertion. Binary searches bound the
render scan to the visible half-open time window. Individual quote notionals are
filtered before layout: up to 4,096 qualifying trades draw separately; overflow
uses side-specific 6 px bins, widening only if occupied bins exceed the circle cap
(see [bubble display semantics](AGENT_API.md#trade-bubbles-gpu-chart)). Larger
circles draw before smaller ones. Layout scratch and QSG vertices are preallocated.
Normal screen-sized fine grids use direct indexing; larger grids use avalanched
integer cell keys (never masked `std::hash<double>` mantissa bits). Fine occupancy
is built once; coarser sizes operate only on occupied cells and stop at the first
fitting 6 px multiple. Tests bound hash probes/cell visits and verify the smallest
supported grid against an independent raw-row occupancy oracle.

A full chart tape, ID index, layout scratch and bubble geometry use roughly
**16–18 MB per chart** with short exchange IDs (about 18 MB with the current fine
cell index); longer IDs and allocator overhead can increase this. The tape is
retained while Trades is off. Scratch/geometry is allocated once per scene-graph
root, with one bubble mesh regardless of layer order.

`TradeBubbleOverlayItem` is a sibling of UGR and the candle item: z=2.5 by default,
or z=1.5 when `tradesAboveCandles=false` (heatmap z=1, candles z=2). UGR publishes
plain C++ frame values; the sibling consumes them via a direct
[`afterSynchronizing`](https://doc.qt.io/qt-6/qquickwindow.html#afterSynchronizing)
callback after all item updates, while the GUI thread is blocked. Render callbacks
retain shared C++ data and typed node links, never traverse GUI QObject graphs;
root destruction clears its node link, and moving windows creates a separate link.
An unchanged visible set at unchanged scale reuses the mesh, with a clipped QSG
translation for horizontal follow-live pan (price changes rebuild); out-of-window appends do not rebuild it.

Session history survives timeframe/renderer changes and same-symbol reconnects;
symbol switches clear it. This is partial observed-session coverage, not a raw
trade-history service. Coinbase subscribe snapshots/reconnects can replay recent
executions. The chart deduplicates nonempty `trade_id` values against IDs in its
retained ring, independent of payload differences. Eviction also expires the ID;
clear/symbol switch resets the ID namespace. The ID set is bounded to 100,000 rows
and allocates only at GUI ingestion, never during rendering. Missing IDs remain
separate rows (time/price/size equality cannot establish execution identity), so
legacy producers can still cause duplicate bubbles on reconnect.

History sources today:
- `ServerDataModel::m_recentFootprintTrades`: server-global RAM, retention
  `clamp(2 * maxConfiguredTimeframe * max(1024, gridWidth), 5 min, 24 h)`;
  `collectOverlayTrades` copies bounded windows for aggregate publishers only.
- `TradeOverlayPublisher`: footprint/VP aggregates and TPO letters; TPO's older
  REST-candle range fallback contains no individual executions, sizes or sides.
  `trade_overlay` is also the error context, not a raw-row response family.
- `JournalTradeEventSource`: recorded trades come from RAWL2 only, with preserved
  trade IDs, millisecond time, aggressor sides and capture gaps. The CLI also
  imports CSV. There is no binary tick logger/reader or legacy history fallback.

`trade_history_request` pages raw event-time executions from this RAM tape when
the requested window is retained, and otherwise from RAWL2 through FlowStore's
journal reader on the two-worker history pool. Disconnects and book resyncs move
the RAM floor to the first subsequent live trade; earlier data comes from the
journal. A crossing window reads the journal through the floor's millisecond
and RAM from that millisecond, then sorts by exchange ms and deduplicates
nonempty trade IDs. No footprint or candle aggregate is used to reconstruct
trades. One trade-history job is in flight server-wide, independently of the
one-second flow replay gate, leaving the second history worker available for
flow and candle history. Its binary reply shares the bounded chunk write queue.
The global history queue remains capped at eight jobs. Requests must name a configured
product and a half-open `[start_ms,end_ms)` window of at most 3,600,000 ms.
More than 250,000 candidate executions or an invalid request returns `refused`
with a `reason`. An occupied trade-history job or full worker queue returns
`refused` with `reason: "busy"`; clients may retry it. Journal gaps, changed
journal sources, invalid live rows, or unavailable early/late coverage return
`partial`; a changed journal source discards that journal portion. A source
still warming may return `building`. Coverage never extends
beyond the current exchange time even when `end_ms` is in the future.

The request is JSON, for example
`{"type":"trade_history_request","req":9,"symbol":"BTC-USD","start_ms":1000,"end_ms":2000}`.
The binary `trade_history_chunk` is `TRD1`, followed by a little-endian u32
JSON-header length, UTF-8 JSON header, little-endian u32 row count, then rows.
The header carries `type`, `schema_version: 1`, unsigned `req`, `symbol`, echoed
`start_ms`/`end_ms`, `status` (`complete`, `partial`, `building`, `refused`),
`coverage: {from_ms,to_ms}` and `count`; refusals also carry `reason`. Each row
is little-endian u16 ID byte length, ID bytes (at most 128), i64 exchange ms,
f64 price, f64 base size, u8 aggressor side (0 buy, 1 sell, 2 unknown), and u32
count (1 for an individual trade). The frame is capped at 48 MiB. The client
rejects truncated frames, count/length mismatches, invalid values, out-of-order
times and trailing bytes before emitting a page. `server_config.flow` announces
`trade_history: true`, `trade_history_encoding: "TRD1"`,
`trade_history_max_window_ms: 3600000` and `trade_history_max_trades: 250000`.
TradeBubbleFeed inserts accepted history rows into the existing 100,000-row
event-time tape; live and history resends of a retained nonempty ID occupy one
row. The GUI does not request these pages automatically yet.

## Independent trade-overlay publication

Footprint, TPO and volume profile no longer run from `heatmap_slice` callbacks.
A session timer dispatches at most one overlay job per second, with one job in
flight per session, up to eight pending history requests and sixteen overlay
symbols. Live symbols rotate; pending history alternates with live work so neither
can monopolize a session. Jobs use the bounded history pool (two workers, eight
jobs globally). Trade snapshotting, sorting, aggregation and wire encoding run
there, never on the network executor or recording live callback. Only immutable
bounded replies return to the session executor. Closing/unsubscribing discards
late replies, and selection generations prevent obsolete grids from publishing.

Each job copies at most 250,000 trades inside its requested half-open time window
and produces at most 8 MiB. Trades outside that window do not consume the budget;
only matching trades are sorted. Oversized live and footprint windows fail explicitly
instead of producing a partial profile. TPO history keeps the newest 250,000 trades
by exchange timestamp and extends the existing approximate candle-range fallback
through the last omitted timestamp (including timestamp ties at candle boundaries).
The seven-day candle input budget remains in force, and each limited page logs its
symbol, window and kept trade count. History
pages contain at most 512 columns. Saturation rejects history requests with a
`trade_overlay` error; live refreshes coalesce until the next timer turn. A live
refresh sends the forming bucket and, when a boundary was crossed, the preceding
closed bucket. Long stalls do not enqueue a bucket backlog; older ranges use
history requests. Footprint uses UTC epoch-aligned half-open buckets; TPO letters
and brackets align to its own session open. TPO history ends at session close,
and live TPO never publishes periods outside the session. VP aggregates the current
TPO session, or the UTC day for W1/M1.

The existing `footprint_slice`, `tpo_slice`, `volume_profile_slice`, and history
chunk wire families retain self-describing grid metadata. Overlay history requests
may additionally specify `price_min`, `tick_size`, and `rows`; defaults come from
`trade_overlays`, independently of heatmap settings. History and live publications
share the selected overlay grid. Weekly (`session_type` 5) and monthly (6, the
UTC calendar month) TPO columns use the same rows at a 5x / 10x coarser tick,
centred on that grid and aligned to their own tick (`tpoGridFor`), so a whole
week or month fits; footprint and VP keep the base grid. The GUI maps footprint
time slots, TPO session columns and VP prices through their own metadata and the
common viewport. Clients page W1/M1 TPO history in windows of at most 7 days.
W1 is the crypto week: 7 days from Monday 00:00 UTC. `tpo_history_request`
requires a unique `request_id` string (1-64 characters; a request without one is
rejected). The server echoes it on the `tpo_history_chunk` and on `trade_overlay`
errors for that request. The GUI keeps one page in flight and accepts only the
chunk carrying that page's id; any other chunk is dropped before a slice is
emitted. `tpo_history_cancel` (`symbol`, `request_id`) drops a queued page or
stops a running one between REST calls; a cancelled page gets no reply. The GUI
sends it for pages it abandons (45 s timeout) or supersedes (new selection).
TPO candle fallback uses the coarsest Coinbase granularity that divides the TPO
period and is aligned with the session open (for example `THIRTY_MINUTE` for
30-minute periods, so a 7-day page is one REST call); every candle lies inside
one period, so each period's range is exactly the high/low of its candles.
The live volume profile follows the TPO session when that session is 24 hours or
shorter (NY, London, Asia, Australia, H24); for W1 and M1 it covers the current
UTC day, because the live trade tape is retained for at most 24 hours.

Live overlays and footprint history derive from the retained server trade tape.
TPO history additionally approximates older price rows from REST candle high/low
ranges before the complete selected tape, including a partially covered boundary
candle. This covers both pre-retention time and trades omitted by the budget. With
no retained tape (for example after restart), candles cover the entire requested
history window. Fetches run on the history worker, in pages of at most 350 candles;
fetch errors fail the request explicitly. Every REST call has one 10-second total
network deadline covering DNS, connect, TLS and HTTP, including any auth fallback.
Timeouts return a failed result and log a warning. Overlay jobs check cancellation
before and after each page; shutdown, unsubscribe and selection changes prevent
additional page fetches. An in-flight page may use the remaining request deadline. Candle history uses
the same joined history pool, so server shutdown cannot destroy the REST client
while a fetch is running. Successful DNS endpoints are fresh for five minutes
and remain usable while a shared refresh is pending; an uncached request waits
for resolver capacity within its own deadline.
Candle ranges and retained trades map
onto the same independent overlay grid, anchored from a candle close when no
trade is available. This preserves session history without introducing persisted
trade-history storage. Footprint and VP still have no observations outside the
retained tape; an empty profile does not establish complete historical coverage.


### Server feed admission and lifecycle (D-b2b)

The server admits only captured products from `rollerProducts()`:
`roller_shadow.products`, falling back to normalized `default_symbols` when empty.
It checks membership before a Session stores a subscription, creates availability
state or sends an acknowledgement. An uncaptured product produces only:

```json
{"type":"error","context":"subscribe","code":"invalid_product","symbol":"XRP-USD","message":"Cannot subscribe to XRP-USD: invalid product."}
```

The refusal has no `max_connections` field. The client emits `subscriptionRefused`
through the queued data-source boundary to the GUI and Agent API diagnostics;
the previously held chart subscription remains active. Subscribe/unsubscribe
symbols are normalized uppercase. Chart lifetimes do not acquire or release feeds.
The server no longer sends Coinbase latency messages.

`sentinel_live_feed_up{product}` and `sentinel_live_feed_up_total` /
`sentinel_live_feed_down_total` follow each captured product's live journal feed.
A1b pages after five minutes down even if history is healthy from file catch-up.
No `sentinel_mdc_*` series is exported.
