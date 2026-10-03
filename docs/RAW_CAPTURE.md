# Pristine Coinbase capture

`sentinel-capture` opens its own Coinbase Advanced Trade WebSockets through
`MarketDataFeeds`. Each requested product has its own connection, engine and RAWL2
v1 stream (`docs/research/2026-10-per-symbol-connections.md`). Each connection
subscribes `level2`, `market_trades` and `heartbeats` for its one product. One
process holds all products; they share one I/O thread and one disk queue pool.
It has no dependency on sentinel-server, its recorder or the GUI. Its local Unix
fan-out streams journal records to consumers (see "Live fan-out" below); rollups
remain outside capture. Core uses no GUI Qt.

## Start, stop and verify

From the repository root, with T7 mounted, build and start a detached 24-hour run:

```sh
cmake --build --preset mac-clang -j 6 --target sentinel_capture
nohup ./build/mac-clang/apps/sentinel-capture/sentinel-capture \
  --root /Volumes/T7/sentinel-data/raw-l2 --symbol BTC-USD --duration 86400 \
  </dev/null >/dev/null 2>&1 &
capture_pid=$!
echo "$capture_pid"
```

For the owner's seven products (seven connections, one process):

```sh
./build/mac-clang/apps/sentinel-capture/sentinel-capture \
  --root /Volumes/T7/sentinel-data/raw-l2 \
  --symbols BTC-USD,ETH-USD,SOL-USD,FARTCOIN-USD,PEPE-USD,DOGE-USD,AVAX-USD
```

`--symbol` is repeatable; it can also be combined with `--symbols`. Inputs are
trimmed, validated, deduplicated and sorted. With neither option, the default is
BTC-USD. Every product is written as RAWL2 v1 with its own `run_id`, its own
connection ids and no routing receipts. Up to 32 distinct products are accepted.
Connects and subscribe batches are paced at one per second each, per process, so
seven products are all subscribed about 7 s after start. Heartbeats remain
connection-scoped (the outgoing heartbeat subscription has no `product_ids`).

RAWL2 v2 (several products on one connection, with routing receipts) was written
from 2026-09-30 to 2026-10-02 only. The writer is removed; `--verify` still reads
and checks that archive, and a root that holds both layouts verifies as one
archive (see "RAWL2 v2 archive (read-only)").

Omit `--duration` for continuous capture. Keep the Mac awake for the measurement
(e.g. `caffeinate -i -w "$capture_pid"` in another terminal). The process exits
cleanly when the duration expires, or stop it early with:

```sh
kill -TERM "$capture_pid"
wait "$capture_pid"
```

Use the printed PID if stopping from another shell. SIGTERM/SIGINT only set a
signal-safe flag; the main loop stops/joins the producer, drains accepted records,
then seals and fsyncs the last block and index. SIGKILL/power loss cannot drain RAM.
The duration starts after metadata retrieval and engine startup; inspect the
report for actual frames, disconnections and gaps before accepting the 24-hour run.

Every run logs through SentinelLogging to
`~/Library/Logs/Sentinel/sentinel-capture-latest.log`, including one cumulative
`Capture stats: product=... conn=... up=... storedFrames=... fileBytes=... queuedBytes=...`
line per product and one `Capture queue: usedBytes=... poolBytes=... floorBytes=...`
line once a minute. Live values are on `GET 127.0.0.1:8091/metrics`
(`ops/monitoring/README.md`). `SENTINEL_LOG_DIR`, `SENTINEL_LOG_KEEP` and
`SENTINEL_LOG_STDERR` work as for sentinel-server. No secondary diagnostic log is
created. A disk error, queue overflow or oversized frame in any product makes the
process exit nonzero with `Capture incomplete`, naming the product. Each product
session reserves 4 KiB, outside the pool, for a final stop/gap
record containing the reason and the first dropped frame's system/steady receive
times and connection ID. Accepted data drains before this marker on overflow.
After an I/O failure the damaged segment is left untouched and a fresh segment
is attempted for the marker. Reserved provisional positions are never reused, so
block-ordinal holes immediately after an abandoned segment are expected. The
verifier's missing-block diagnostic describes that already-counted damage
(bad tail/explicit gap), not a separate loss event; it does not make the run clean.
If the volume is still unwritable, even that marker
cannot be persisted: the run log explicitly says so and exit remains nonzero.
Check the exit status and the run log.

Verify the whole capture root to check every product and routed frame together.
A product directory also preserves snapshot and sequence context across hours,
but cannot check the raw bytes referenced in other product directories:

```sh
./build/mac-clang/apps/sentinel-capture/sentinel-capture \
  --verify /Volumes/T7/sentinel-data/raw-l2
```

`--verify` also accepts one `.rawl2` file. It is offline and read-only; stdout is a
JSON report. Exit 0 means no observed integrity failures or open runs within
the supplied scope (including tails excluded by scope); **3** means `ok && !complete` (only the newest run remains open/in progress);
2 means capture gaps, corruption, interrupted older runs, missing snapshot anchors,
missing streams/segments or other failed invariants, and 1 is a fatal invocation error. Existing files are
never repaired or rewritten by verification. Add `--strict-trades` to also return
2 for unfilled trade-id gaps; by default upstream/reconnect trade omissions are
reported separately and do not fail archive integrity.

`ok` is integrity of the supplied scope, while `complete` additionally requires
closed runs or a tail explicitly excluded by the query. `truncated_by_scope`
on a run (and `truncated_by_scope_runs` in aggregates) means inventory found a
later segment of that same product/run outside the selected scope. Such a tail
is neither open nor interrupted and does not itself fail verification; it does
not certify the unselected suffix or excuse missing start/snapshot context.
The last selected file of a truncated run is nonterminal in the archive: it must
be sealed/indexed. An unindexed or torn selected tail increments `bad_tails`
and fails verification even when its successor is outside the query.
`routing_checks_deferred` independently identifies product-only
scans that cannot check other destinations' raw bytes. `ok_closed_runs` includes
both normally closed and interrupted histories. A run with its start but no stop
marker, unless `truncated_by_scope`, has `open: true` / `open_runs > 0` **only if it is the newest run in every
product stream declared by its header**. Newness uses `(run_started_system_ns,
run_id)`, not filenames or mtimes. Otherwise it is `interrupted: true`, increments
`interrupted_runs`, and fails verification. Both open and interrupted runs retain
the legacy `incomplete_runs` count. Discovery checks headers in the
archive root even for a product/month/file query. There is no archive file-count
limit. Selected paths are externally sorted in temporary indexes with at most
1,024 paths buffered, then replayed one file at a time. Temporary disk usage is
proportional to selected paths; in-memory report aggregates scale with the
number of products, runs and receive days, not hourly files. `inventory_files`
and `inventory_peak_buffered_files` expose the inventory count and path-buffer
high-water mark (merge cursors use two additional paths). An open or interrupted
run's last file may have no index or a partial terminal
block/index without failing prefix integrity; complete CRC failures and interior
damage still fail. An open run awaiting its first snapshot is pending, not an
anchor failure, unless it already received unanchored updates. A connection with
**no L2 events for a product** increments `empty_connections`, not
`missing_snapshot_connections`, even if it carried heartbeats, trades or foreign
product frames. Connections with L2 events and no snapshot remain integrity
failures (`missing_snapshot_connections` and `unanchored_l2_events`).
`empty_connections` totals sum product/connection pairs. With one connection per
product (v1) each pair is one real socket. In the v2 archive one silent WebSocket
connection across seven products counts as seven. Per-product and aggregate
`empty_connection_details` are independently capped at 30 entries and include
product, run, connection ID, UTC `start_time`, exact `start_system_ns`, and
`duration_seconds` measured with the monotonic clock. End-of-scope connections
use their observed prefix duration; an open run still exits 3 when otherwise
valid. Down/stop/reconnect/EOF boundaries count each connection once. Empty
connections do not enter the shared integrity diagnostic list.
A single middle hour generally fails
because the start/snapshot context is missing.

**Open does not prove the process is alive.** A crashed newest run is still
indistinguishable from a live writer until another run supersedes it; exit 3
makes that uncertainty distinct from a completed verification. Reverify after
close to certify completion. Files are scanned to their observed lengths;
whole-root digest comparison is deferred for open runs because product writers
flush at different times. Interrupted multi-product runs compare bounded groups
through their common prefix, including a group's available receipts even if a
peer ended before flushing its raw copy. Such a lost copy is a routing failure,
in addition to the interruption itself. The merged unreceipted tail also checks
sequence continuity, including the transition from the last proven group.
For interrupted or scope-truncated runs, comparison finishes the first group in which any stream
ends, including available receipts and merged-tail continuity, then stops.
Later groups from longer peers are not cross-compared: their absent peer
coverage cannot justify an additional missing-copy error for every frame.
Local per-product replay still checks all selected files. A multi-product run
queried through one product/month is scope-truncated as appropriate and defers
peer proof. A stable query containing multiple products is its own inventory
root, but live rotation between file selection and inventory can still expose
later segments outside the selected list. Cross-stream comparison treats that
scope-truncation flag like interruption for its tail, without marking the run
interrupted.
Routing anomalies increment `routing_errors` individually; `routing_details`
keeps the first 30; each `connection_runs` entry also has its routing-error count.
Checking continues at subsequent group boundaries after
an anomaly. Unsequenced broadcasts are ordered by steady time and per-stream
position for routing proof; they still count as unsequenced input in the
integrity report.

Two routing-comparison limitations remain. Recovery advances by group index,
not by matching boundary identities. If a stream loses a boundary record, later
groups remain misaligned and report errors; `routing_errors` then counts failed
comparisons of misaligned groups, not distinct underlying anomalies. This can
inflate the count but does not hide the loss.

Independent merge heads with exactly equal steady-clock timestamps are ordered
by their routing key, with unsequenced keys after sequenced keys. If an
unsequenced frame heads one stream while a sequenced frame heads another at
that exact timestamp, this tie-break can differ from arrival order. Because
the receipt digest includes order, it could theoretically report a false
mismatch. This contrived ambiguity is not currently resolved by the verifier.

Header creation itself is not an atomic read snapshot:
retry if a concurrent new file has an incomplete header.

The report includes mean frames/s, p99 counts in one-second steady-clock buckets
(including idle seconds and partial end buckets), received bytes/day, zstd
bytes/day, actual file bytes/day including framing/indexes, all-channel sequence
gaps, explicit capture gaps (with reason and first-loss timestamps),
connections/reconnects, acknowledgments, resync/invalidation counts, snapshot
frame sizes and entry counts (including zero entries), and received bytes per
channel. Rates use the sum of recorded per-run steady-clock spans; downtime
between process runs is excluded, disconnections within a run are included.
Compressed blocks mix channels, so compressed bytes cannot be attributed exactly
to individual channels.

`products["BTC-USD"]` (and each other product) includes `frames`, `l2_events`,
`replayed_l2_events`, `snapshots`, `received_bytes`, `file_bytes`, `zstd_bytes`,
`*_bytes_per_day`, channels, sequence/anchor failures and run status. Its frame
and received-byte counts cover the **whole raw envelopes physically routed to
that product**, including shared control traffic and other events inside a mixed
product envelope. `l2_events` counts only that product's events. Receipts have a
separate `frame_references` count, not extra received frames. `days[YYYY-MM-DD]`
contains actual UTC receive-day frame/byte/L2-event counts and physical file bytes
attributed to the file's opening UTC day. `*_per_day` remains a rate extrapolation
from steady-clock duration, not the actual daily totals.

`totals.frames` and `totals.received_bytes` count each incoming frame once per
connection/run; `stored_frames` and `stored_received_bytes` include physical
routing duplicates. Total file/zstd bytes sum the actual per-product files,
including receipts, headers and indexes. Total duration counts each shared run
once (concurrent independent runs each contribute their own span). During an open
run, connection totals are a lower bound: completed proof groups plus the largest
observed local raw tail (`totals.counts_in_progress: true`). A run's
`pending_routing_frames` names raw frames still awaiting a receipt in its
representative stream. Totals' L2 and
disk counts sum the product reports; `totals.days` follows the same unique-frame
and physical-disk rules. Existing single-product top-level verification keys and
closed-run meanings remain; `products`, `totals` and status fields are additive.
Multi-product top-level counters mirror totals; detailed snapshot-size and p99
statistics remain in each product report.

## Trade continuity and CVD sanity totals

The offline verifier checks `market_trades` independently of WebSocket sequence
and archive integrity. RAWL2 v1/v2 bytes are unchanged. Coinbase's captured
snapshots and update batches contain descending IDs, so each selected-product
event is sorted numerically. IDs are unsigned 64-bit decimal strings, including
`UINT64_MAX`. Trade sizes are plain positive decimals, independently of the L2
`base_increment` grid. Signs, exponents, zero and empty decimal parts are invalid.

A forward jump creates a **candidate**, not an immediate error. The verifier
retains exact missing ranges and removes IDs as they arrive later, including
sparse old updates, out-of-order frames, mid-connection snapshots and later
connections/process runs. Final classification happens at the end of the archive
scan. `filled_later` counts recovered candidate IDs; `still_missing` counts IDs
still absent. Filled candidates are discarded. Partial fills split the exact
backfill ranges; they do not clear an entire candidate. Category gap counters
count candidates still containing at least one missing ID, not split fragments.

* `upstream_trade_gaps` / `upstream_missing_trades`: unfilled connected gaps with
  no evidence of capture damage in their originating connection. These never
  change `ok`, `ok_closed_runs`, normal exit status, `errors` or shared `details`.
* `integrity_trade_gaps` / `integrity_missing_trades`: unfilled gaps whose same
  originating connection has a WS sequence gap, explicit capture-gap marker or
  bad tail/read failure. Damage arriving after the trade gap is included. These
  remain archive-integrity errors. Damage in another connection does not
  reclassify a clean connection's candidates. A damaged tail is attributed to
  its last observed connection when no later identity is readable. An unsealed
  newest live prefix alone is not damage; an unsealed superseded run is.
* `reconnect_trade_gaps` / `reconnect_missing_trades`: the uncovered interval
  from the previous connection's highest ID to the first new higher ID. This
  includes downtime between process runs; it does not fail archive integrity.
* `snapshot_trade_gaps` / `snapshot_missing_trades`: internal gaps in initial
  recent-history snapshots. Mid-connection snapshots after updates contribute
  trades and cannot conceal a forward hole. Snapshot overlap is deduplicated.

Each category has its own `*_trade_gap_details`, capped at 30 entries. Each
product's **uncapped** `missing_trade_ranges` contains every remaining fragment:
`product`, string `first_id` / `last_id` (inclusive), `count`, `category`,
`approx_time` (the following observed trade's exchange time), receive nanoseconds,
run UUID and connection ID. This list is suitable for exact-ID backfill requests.
It scales with unfilled holes, not trade history; unlike bounded diagnostics it
is never silently truncated. Pending bookkeeping likewise scales with the
required backfill output; fully filled candidates release their storage.

`trade_tape_complete`, per product and overall, means no detected candidate IDs
remain missing in the selected scope. It does not alter archive `complete` or
`ok`. `--verify <path> --strict-trades` changes exit status to **2** for any
unfilled category; its JSON integrity fields remain unchanged. Normal verification
still returns 0 for clean closed archives or 3 for a clean open newest run.
`--strict-trades` without `--verify` is rejected. Malformed trade data still fails
integrity; this policy exception applies to missing IDs, not invalid JSON/scalars.

**Coverage boundary:** no completeness claim precedes the first observed anchor
or extends beyond the scanned prefix. Reconnect snapshot IDs at or below the
previous high watermark only fill already detected candidates; the verifier does
not infer new holes inside this sparse old-history region. `reconnect_overlap_check`
states that scope in each product report. Initial-snapshot holes and forward jumps
above the previous high are checked. This distinction prevents sparse historical
update batches from manufacturing gaps in previously captured history.

Exact ID deduplication uses compressed intervals. Intervals ending more than
**10,000,000 IDs** below the high watermark are evicted; there is no run-duration
or 65,536-interval failure limit. The numeric window bounds retained disjoint
intervals (at most about 5,000,002), and `dedup_window_evictions`,
`dedup_retained_intervals` and `dedup_peak_intervals` expose its behavior. A retained
interval may still prove an older duplicate. Otherwise an older observation is
counted again and increments `dedup_out_of_window_trades`: unique-trade/volume
claims must account for that uncertainty. Eviction never discards missing-range
bookkeeping; even a very old fill reconciles a candidate exactly.

Products report deduplicated `trades`, `duplicate_trades`, normalized UTC
`first_trade_time` / `last_trade_time`, `buy_trades`, `sell_trades`, and decimal
`buy_volume` / `sell_volume` in base currency (snapshots included). Volumes use
50-digit decimal arithmetic. Totals sum product counts, not unlike currencies.
Coinbase's [`MarketTrade` schema](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/advanced-trade-asyncapi.json)
defines wire `side` as **maker** side. Report buy/sell is **aggressor** side:
wire `SELL` adds buy volume, wire `BUY` adds sell volume; CVD is buy minus sell.

Trade extraction uses the routing SAX parser, retaining selected-product scalars
for the current bounded frame. V1 does no routing SHA-256 or destination sorting:
a cheap channel peek selects the trade SAX path or one reusable DOM parse for L2
and other envelopes. Missing channels retain `<unclassified>`; malformed JSON
fails before sequence accounting and does not increment `unsequenced_frames`.

### Observed feed omissions (archive audit, 2026-10-02 UTC)

The real archive does **not** prove an unbroken trade tape. Independent
read-only decompression and ID search found none of these 16 IDs anywhere in
the selected products' archived frames, while Coinbase's public REST history
returns them as executed trades:

| Product | Missing IDs (inclusive) | Count | Exchange time (UTC, 2026-10-01) |
|---|---|---:|---|
| BTC-USD | 1100950971..1100950974 | 4 | 14:55:31.878671..14:55:32.106460 |
| ETH-USD | 846432842 | 1 | 14:55:31.789758 |
| DOGE-USD | 175259062 | 1 | 14:55:31.808308 |
| BTC-USD | 1101035040..1101035049 | 10 | 17:28:59.845871..17:29:00.059361 |

Reproduce the external checks with public, credential-free REST requests:
[BTC first interval](https://api.exchange.coinbase.com/products/BTC-USD/trades?after=1100950976&limit=10),
[ETH](https://api.exchange.coinbase.com/products/ETH-USD/trades?after=846432844&limit=4),
[DOGE](https://api.exchange.coinbase.com/products/DOGE-USD/trades?after=175259064&limit=4),
[BTC second interval](https://api.exchange.coinbase.com/products/BTC-USD/trades?after=1101035051&limit=14).
The archive has no WebSocket sequence discontinuity or routing-proof failure at
these points. The simultaneous first three omissions and intact envelope
sequence are evidence of an upstream feed omission, not dropped capture files;
the verifier cannot identify Coinbase's internal cause. Do not infer trade
completeness from envelope continuity alone. A future backfill consumer must
reconcile these IDs before claiming exact CVD for the affected intervals.

An initial audit also exposed an incorrect verifier assumption: Coinbase sends
snapshots mid-connection, sometimes followed by sparse historical `update`
batches. For example, BTC connection 5 in run
`61fdb803-bedb-4812-88ff-a288de87ea71` updates through ID `1100439303`, then
sequence `140975` is a snapshot containing `1100439304..1100439314`, and
sequence `140978` updates with `1100439315..1100439316`. Discarding that snapshot
would invent an 11-trade gap. The verifier includes those snapshot trades and
counts historical overlap as duplicates; deterministic fixtures cover this
pattern and ensure a snapshot cannot hide a real forward hole.

## Configuration and limits

The capture has CLI options only; it does not load or modify server/client YAML.

| Option | Default | Meaning |
|---|---|---|
| `--root` | `/Volumes/T7/sentinel-data/raw-l2` | Absolute storage root; must resolve to a mounted volume |
| `--symbol` | `BTC-USD` if no product options | Repeatable Coinbase product |
| `--symbols` | none | Comma-separated products, combined with repeated `--symbol` |
| `--block-ms` | `1000` | Block age target, measured from receive steady-clock time; 1..60000 ms |
| `--block-bytes` | `1048576` | Uncompressed byte target; an individual frame remains whole |
| `--fsync-blocks` | `1` | Sync every N blocks; 0 syncs at file close only |
| `--zstd-level` | `3` | 1..19 |
| `--queue-mib` | `512` | Shared disk queue pool for all products, including record overhead; 1..4096 MiB. Accounting only: bytes are allocated as frames queue, never up front |
| `--queue-floor-mib` | `2` | Per-product part of the pool that no other product can take; 0..256 MiB. Products x floor must not exceed `--queue-mib` (startup error) |
| `--metrics-port` | `8091` | Prometheus `/metrics` and `/ping` on 127.0.0.1; 0 = no listener. A bind failure logs a warning and the capture continues |
| `--duration` | `0` | Seconds until clean stop; 0 waits for a signal |
| `--key-file` | `key.json` | Existing optional Coinbase credentials |
| `--jwt` | off | Enable existing engine JWT auth; public channels need no credentials |
| `--ca-bundle` | `resources/certs/ca-bundle.crt` | Existing REST/WS TLS CA bundle |

The root never falls back to another disk when `/Volumes/<name>` is absent.
The server's `/Volumes/T7/sentinel-data/recording` tree is refused, including
canonicalized symlinks. One process holds every requested per-product lock
(including overlaps with single-product runs). All metadata is fetched before
starting the engine using `CoinbaseRestClient`, retaining `quote_increment` and
`base_increment` strings and the complete product JSON. Metadata failure prevents
startup; no WebSocket is opened with guessed increments. Credentials/JWTs are
never placed in the capture header.

The ingest observer is called before parsing and only stamps/copies records into
that product's bounded queue. Each product has its own disk worker
(`capture-disk` thread), one zstd compression context, and handles its own writes
and sync. All queues account to one shared pool (owner decision 7, 2026-10-02):

- The pool is accounting only. A record is allocated when it is queued and freed
  when it is written, so a healthy capture holds about 0 queued bytes. Nothing is
  reserved up front. At the measured ~98 KB/s for seven products, the 512 MiB
  default covers about 90 minutes of stalled disk I/O (FM-127 was 39 minutes).
- Each product may always use its floor (`--queue-floor-mib`). Bytes above the
  floor come from the shared remainder, total - products x floor, first come first
  served. A product that floods the remainder fails itself; it cannot refuse
  another product's frames below that product's floor.
- When a disk worker fails, its queued records are counted as lost (the gap
  marker keeps the first one) and their bytes return to the pool at once, not at
  close, so a failed product never makes a healthy one fail for "pool limit". The
  backlog is detached under a short lock and freed outside it, so the shared
  market-data I/O thread never waits on that cleanup.

**One failure stops the whole process.** This is the restart contract for every
product failure (pool full, record over 16 MiB, disk error, ingest exception):

1. The failing product logs `Capture failed: product=<id> error=<reason> ...
   queuedBytes=... poolUsedBytes=... poolBytes=... floorBytes=...` once, right
   after the session lock is released, in the thread that saw the failure. It
   goes through the normal log sink, which writes to the internal disk and can
   itself block; it never runs under the session lock, so a slow sink cannot
   stall the ingest thread's submits. Later records of that product are refused
   and counted as lost; its accepted data still drains first on a pool overflow.
2. Within 100 ms the supervisor logs `Capture stopping every product after a
   failure: error=...` and stops all feeds. Every other product closes its run
   with stop reason `capture failure`, so all products have a gap until restart.
3. After the drain, `Capture incomplete: error=...` is logged and the process
   exits 1. launchd restarts it after `ThrottleInterval` (30 s); each product
   starts a new run.
4. If disk I/O is frozen (FM-127), step 3 can block in the drain/join: there is no
   exit and `/metrics` stops answering (the main thread is in the join), so alert
   A3 pages for `sentinel-capture`. The lines from steps 1 and 2 are already in
   the run log on the internal disk.

The pool is a memory budget that delays this stop, not a loss guarantee.
- After a disk failure the damaged segment is abandoned and the gap marker goes
  into a new segment. A failure outside a writer operation flushes and seals the
  healthy buffer first.
Sync uses the shared persistence primitive: `F_FULLFSYNC` on Darwin with `fsync` fallback where full sync is unsupported. A record
is limited to 16 MiB (the existing Beast transport's default message limit), a
block to that record plus framing, and an index to 65,536 entries; a new segment
starts if that index limit is reached. Peak capture memory includes the queued
records (at most the pool), one in-flight record per product, per-product
raw/compressed block buffers and indexes (bounded by 32 products), and the existing
engine's transport/JSON parser buffers. Writer failure or queue saturation ends
the capture with an explicit error. Fsync cannot recover bytes still in the
queue or current block; with defaults, block flush is targeted at one second or
1 MiB, plus disk scheduling delay.

Each product's engine retries its own transport failures (including initial
handshakes) with 1 s exponential backoff capped at 30 s, plus 0..1 s jitter,
resetting at the first accepted snapshot. A sequence gap, 20 s without any frame,
30 s without L2 while heartbeats flow, malformed L2 or a provider error reconnects
only that product. Connect and close have bounded deadlines in the transport, so
the old whole-process 60 s restart supervisor is removed. A feed down for 2
minutes logs `Feed down: downMs=...` once a minute, and alert A4 pages
(`sentinel_capture_feed_down_seconds > 120`).

## RAWL2 v2 archive (read-only)

This section describes the layout written 2026-09-30..2026-10-02, when one
connection carried all products. The writer is removed; the verifier keeps this
reader so the archive stays verifiable. Tests build v2 files with a test-only
writer (`tests/capture/legacy_v2_fixture.cpp`). New captures never contain kind 9
records, `connection_products` or `routing`, and the production writer refuses
that metadata.

Files retain `<root>/<product>/YYYY/MM/DD/HH.rawl2` and exclusive-create collision
segments. Multi-product writers share a run UUID and run-start timestamp, but
segment/block ordinals are per product: different payload sizes produce different
block boundaries. All lifecycle/error/invalidation/resync markers go to every
product with identical payload, receive clocks and connection ID. Any invalidation
clears every replay book; only that product's new snapshot re-anchors it.
Transport-up alone starts a new connection/sequence domain.

Known `l2_data.events[].product_id` and
`market_trades.events[].trades[].product_id` select destinations. A frame naming
two products is written **unchanged** in both; it is never split, reserialized or
filtered. Product IDs must exactly match the subscription set: there is no
case folding, USD/USDC substitution, alias resolution or implicit subscription.
If any relevant ID is unsubscribed, an alias, missing, duplicated ambiguously or
of the wrong type, routing conservatively broadcasts the whole frame. Acks,
heartbeats, unknown channels, unclassified envelopes and malformed JSON also
broadcast. No raw frame is discarded. An unexpected L2 product fails book replay
rather than being silently attributed to another book; trades are retained as
raw envelopes. Routing uses a bounded SAX parser on the disk thread: it checks
JSON syntax but never builds the snapshot's `updates` array or copies its price
and quantity strings into a DOM. The engine's own parser is unchanged.

Each stream contains its own raw frames plus **range receipts**, not one receipt
per foreign frame. A proof group ends after at most 60 seconds or 65,536 incoming
frames, before every lifecycle marker, connection change or UTC hour change,
and at stop. Every stream then receives one kind-9 summary for the same group.
It covers the entire connection range, including interleaved local frames, and
accounts for all foreign frames with one digest. Raw data keeps the configured
block/flush/fsync cadence (default one second); proof groups can span those
storage blocks. The receipt itself is compressed in an ordinary block. Only
counts and an incremental hash are held while producing it, not frame history.

This generalizes coalescing adjacent foreign runs: at 22 BTC frames/s those runs
would still produce thousands of SHA-256 values per minute, and a 1-frame/s
product would otherwise pay for the whole connection's entropy in each storage
block. Amortizing the hash over a bounded connection group meets the disk budget
without delaying raw data durability. The tradeoff is that the latest raw tail
can precede its routing proof by up to 60 seconds (plus scheduling/flush delay).
A clean close always finalizes the proof. Single-product replay remains local;
root verification reconstructs groups by sequence rather than scanning unrelated
snapshots into the product's book.

V2 changes only the magic's last byte (`RAWL2\r\n\x02`), `format_version: 2`,
additional header fields, and permission for kind 9. Block, record, index and CRC
framing are identical to v1. Old readers reject v2 instead of silently losing
sequence proof. All captures since 2026-10-02 write v1, with no v2 header fields
or record kinds. V2 retains the file's own `product_metadata` and single-entry
`products`; `connection_products` is the sorted, unique full subscription set.
The routing identifier is frozen as **`"product-ranges-v2"`**. The experimental
`product-receipts-v1` layout was never deployed to real data and is rejected;
future semantic changes require another routing ID.

Kind 9 uses the **last frame's** two receive clocks and connection ID. Its compact
UTF-8 JSON fields are:

| Field | Meaning |
|---|---|
| `first_seq`, `last_seq`, `count`, `bytes` | First/last connection sequence, total frames and exact incoming payload bytes in the group |
| `foreign_count`, `foreign_bytes` | Frames/bytes not physically stored in this product's group |
| `destinations` | Map from canonical owner product to `[count, bytes]`; each frame's owner is its lexicographically first raw destination, so these sum to the connection totals without double counting |
| `sequence_gaps` | Producer-observed discontinuities or invalid sequence values within the group; any nonzero value fails verification |
| `sha256` | Lowercase SHA-256 of the concatenated frame identity lines, in connection receive order |

A frame identity is the compact JSON object with `channel`, `products` (sorted raw
destinations), `received_bytes`, `sequence_num`, and lowercase `sha256` of the
**exact raw bytes**. Its hashed line is compact JSON array
`[system_ns, steady_ns, connection_id, identity_object]` followed by LF, with
lexicographically ordered object keys (nlohmann::json default). Golden vectors
pin the routing ID, identities, lines and range digests independently of writer
and verifier implementation. These are integrity hashes, not signatures.

Each product checks range continuity/counts, its raw sequence order and its own
snapshot/metadata anchors. Receipts never supply snapshots or book updates. A
capture-observed connection sequence discontinuity inserts an invalidation in
all product streams before the affected raw frame. Engine invalidation/resync
markers are copied to all streams for lifecycle accounting, but the verifier
invalidates only the named product's book. An empty or absent `product` means
connection-wide invalidation. Product-scoped recovery never supplies a snapshot
or invalidates another product's replay book.

Whole-root verification requires every declared stream. It incrementally merges
raw identities by sequence for one proof group at a time, checks all expected raw
copies and their exact hashes/clocks, regenerates every product's receipt, and
compares lifecycle markers. Memory is bounded by a group plus one decoded storage
block per stream, not by the run length; snapshot payload capacity is released
after hashing. Missing copies, receipts, changed bytes/clocks, destination/count
mismatches and sequence gaps all fail. Interrupted runs use the available common
prefix and still check a durable receipt against a truncated peer.

A product directory, its descendants (including `<root>/BTC-USD/2026/09`), or a
single file reports `scope: "product"`. Closed product-only scans can be complete
for that scope but report `routing_checks_deferred`: they cannot certify other
products' payloads. `connection_runs[].routing_checked` reports actual whole-set
comparison; open runs defer it. Reconstruct the exact connection by merging the
raw copies by connection/sequence, retaining the original clocks and bytes.

The writer kept receipt overhead under 0.5% of each product's file bytes in its
seven-product benchmark (removed with the writer, 2026-10-02).

## RAWL2 v1 framing (also used by v2)

All integers are little-endian, unaligned. Times are signed, nonnegative 64-bit
nanoseconds from `system_clock` (Unix epoch on supported platforms) and
`steady_clock` (run-local comparison only). CRC is IEEE CRC-32, as in the existing
core CRC utility. Paths use UTC:

`<root>/<product>/YYYY/MM/DD/HH.rawl2`

Files are opened exclusively, never appended on restart. If the hour name already
exists, the name is `HH.<run-uuid>.<segment-number>.rawl2`. Clock rollback also
creates another segment. Verification orders files by run identity and segment
ordinal, not filename or receive wall-clock monotonicity.

- File header: 8-byte magic `RAWL2\r\n\x01`, JSON length `u32`, JSON CRC `u32`,
  UTF-8 JSON. JSON is capped at 1 MiB and includes format/tool/build versions,
  full product metadata and source, products/channels, configuration, run UUID,
  segment number, first block ordinal and opening clock stamps.
- Each block: `BLK1`, compressed length `u32`, raw length `u32`, record count `u32`,
  first/last system timestamp `i64` each, run-wide block ordinal `u64`, raw CRC
  `u32`, header CRC `u32` over the preceding 44 bytes, then one independent zstd
  frame. Blocks have no dictionary or dependency on a previous block.
- Each decoded record: length `u32` (excluding this prefix), kind `u32`, system
  timestamp `i64`, steady timestamp `i64`, connection ID `u64`, remaining payload
  bytes. Kind 1 is the exact pre-parse WebSocket message, including whitespace,
  decimal strings, acks, unknown channels and malformed JSON. Transport ping/pong,
  TLS/TCP framing and outgoing requests are outside this ingest seam. Kinds 2..8
  are transport up, transport down, book invalidated, resync requested, capture
  started, capture stopped and engine error. Their payloads are JSON reasons and,
  where applicable, products. Transport-up increments the run-local connection
  ID; synthetic engine errors never invent a new connection. A failed capture's
  stop payload has `gap: true`, `reason`, `first_dropped_system_ns`,
  `first_dropped_steady_ns`, `first_dropped_connection` and `first_dropped_kind`.
  The verifier counts it in `explicit_capture_gaps` and emits bounded details in
  `capture_gap_details`, independently of sequence gaps or missing markers.
- Closing index: `IDX1`, payload length `u32`, payload, payload CRC `u32`. Payload
  starts with entry count `u32`; each 44-byte entry is offset `u64`, first/last
  system timestamps `i64`, block ordinal `u64`, record count `u32`, compressed
  length `u32`, raw length `u32`. The index is rebuilt from block headers while
  scanning and compared byte-for-byte with a present closing index. System time
  may regress; use all matching entries rather than assuming sorted wall times.

Incomplete terminal block/index data and an unframed zero/garbage suffix are
reported as torn tails and skipped. An unframed suffix is scanned for later valid
block/index framing; finding it proves interior corruption and fails verification.
Complete bad CRCs, invalid lengths, zstd errors and mismatching indexes also fail
verification. Valid prefix blocks remain recoverable. Missing indexes or
start/stop markers are reported as incomplete. A valid open
terminal prefix is distinguished from a closed-run integrity failure as described
above; it includes the ambiguity of a crash exactly between blocks.
The verifier bounds file discovery at 100,000 files and book replay at 2,000,000
levels. Book prices/quantities use checked integer arithmetic with metadata
increments (up to 18 decimal places, 64-bit normalized mantissas/atoms), never
floating point. Off-grid, negative, malformed or overflowing values fail replay;
the original raw bytes remain preserved. Invalidations clear the reconstructed
book until another accepted snapshot; updates during an unanchored interval are
reported rather than treated as observed liquidity.

Offline regressions cover exact bytes, framing/indexes, zero/garbage and truncated
tails, interior corruption, rotation, explicit overflow gaps, and a real short
write followed by failure-marker recovery. Engine tests inject a fake transport
through repeated failures and watchdog resyncs. `CaptureApplicationTests` runs
the production application path with fixture metadata and a fake transport,
sends POSIX SIGTERM during an unfinished block after a reconnect, and verifies
the drained data, connection IDs, stop reason, final index and run log. No test
contacts Coinbase.

Multi-product regressions check the actual outgoing subscriptions for all seven
products (one connection each) through repeated/comma-separated/mixed CLI forms;
uneven reconnects per product; independent v1 runs beside a v2 run; and the
capture `/metrics` endpoint of a running application. Queue-pool regressions
check that floors admit every product under a flood from one, the total cap, that
a failed session returns its bytes at failure, that a 512 MiB pool for seven
products allocates nothing up front, and that products x floor above the pool
refuses to start. V2 verifier regressions (built with the test-only writer) keep
exact routing and receive clocks, a mixed-product L2 envelope, connection-wide
gaps/invalidations, unique totals and daily accounting, mixed v1/v2 runs and hour
rotation, missing streams or altered raw/reference bytes/clocks, frozen
routing/digest vectors, snapshot allocation bounds, superseded interrupted runs
and crash-tail raw loss. Other regressions cover live exit 3 and event-driven
application readiness.
Trade regressions cover descending contiguous batches, connected holes,
reconnect loss, exact snapshot/update deduplication, aggressor volumes and time
precision, product isolation in mixed envelopes, v1/v2, hourly/process boundaries,
invalid scalars and overflowing IDs, and real mid-connection snapshots followed
by sparse historical updates. A mid-connection snapshot cannot hide a forward
hole. Review regressions cover upstream versus capture-correlated failures,
late and partial fills across frames/connections, exact backfill fragments,
independent bounded diagnostics, strict CLI exit status, numeric-window eviction,
fills older than that window, the reconnect old-history coverage boundary,
sub-grid sizes and 18-place sums, interval bridging at `UINT64_MAX`, and legacy
missing-channel/malformed-envelope accounting. A routing allocation test checks
that the v1 header peek stops before the L2 body. Targeted mutation checks disable
these behaviors and must fail their regressions; every restored source is touched
and rebuilt before proceeding.

## launchd arguments (review/deploy separately)

The arguments did not change for per-product connections: the new defaults
(512 MiB pool, 2 MiB floors, `/metrics` on 8091) apply without a plist edit.
Deploy only with `scripts/dev/deploy-runtime.sh capture`.

The following is the exact `ProgramArguments` array for the seven-product service
retaining the current service's runtime executable, working directory and defaults. No service operation is
performed by capture development/tests; existing capture and recorder services
must be left alone until an operator separately deploys a new binary/configuration.
Do not launch this over a currently locked BTC-USD directory.

```xml
<key>ProgramArguments</key>
<array>
  <string>/Users/copeharder/Sentinel-runtime/bin/sentinel-capture</string>
  <string>--root</string>
  <string>/Volumes/T7/sentinel-data/raw-l2</string>
  <string>--symbols</string>
  <string>BTC-USD,ETH-USD,SOL-USD,FARTCOIN-USD,PEPE-USD,DOGE-USD,AVAX-USD</string>
  <string>--duration</string>
  <string>0</string>
</array>
```

The 512 MiB pool is shared by all seven products, each with a 2 MiB floor. Overflow
retains the existing nonzero exit/restart contract; the pool is a memory budget,
not a loss guarantee.
No exchange credentials are required for these public channels.

## Offline HMC2 conversion (slice A)

`sentinel-roll` consumes the v1/v2 archive without changing capture or the running
recorder. It shares the engine's floating-point L2/trade parsers, derives daily
product grids from journal metadata, and drives BookRecorder with one receive-time
tick per record. It supports bounded blocking admission, durable checkpoints,
resume, idempotent reruns, dry-run reports and decoded `hmc2_diff` comparisons.
See [roller commands, recovery rules, measurements and the controlled tick-schedule
comparison](ROLLER.md). Production backfill and service cutover remain separate
orchestrator operations. This slice's outputs are under the agent's `roll-out/`.

## Live fan-out (slice B)

Capture serves a Unix domain stream socket by default at
`~/Sentinel-runtime/run/capture.sock`. Override with `--fanout-socket PATH`.
The socket directory must be owned by the current user and mode 0700; newly
created directories are 0700, the socket is 0600. Paths under `/Volumes`, any
Git checkout (including worktrees), and socket/parent symlinks are refused after
canonicalization. A lock prevents two owners; a stale socket is removed only
under that lock, after a connection probe reports no listener. Regular files
are never removed. A fanout setup failure logs its reason with `sLog_Error`, sets
`sentinel_fanout_running` to 0, and leaves journal capture running for all products.
Setup retries after 30 s, doubling to a 10 min maximum; success resets the delay.
Fixing a directory's mode/ownership or clearing a conflicting listener permits
a later retry to recover without restarting capture. The ring remains bounded
while unavailable; clients recover from the journal if their cursor is absent.
Grafana's **Capture fan-out down** alert fires after the gauge stays below 1 for
five minutes; the existing service-down alert covers a missing capture scrape.

`--fanout-ring-mib` defaults to 32 MiB **per product** and
`--fanout-client-mib` defaults to 16 MiB per connection (both 1..256).
Retention is 60 seconds of monotonic publication age, bounded by ring bytes,
whichever comes first. There are at most eight clients, one product per socket.
A consumer of all seven products opens seven sockets. Ring, ingress and socket
queue accounting is separate from `QueuePool`; no capacity is taken from disk
capture. Each product has a fixed 1,024-slot SPSC ingress queue, also capped at
32 MiB. Payloads are allocated as they arrive, never at capacity up front.

**Publication point:** at append, before disk I/O (including a preceding block's
flush or a new segment's header), the writer hands off each exact framed record
with its final `(product, run_id, block, record)` position and `provisional:true`.
The run UUID, run-wide block ordinal and zero-based record index include size,
time and hour rotation, lifecycle, invalidation, resync and terminal stop records.
The fanout worker never reconstructs or parses the raw payload. Live delivery
of the appended record does not wait for flush. Capture's existing disk-worker
queue can still delay reaching append; this is not an end-to-end feed-age claim.

Each successful block flush emits `durable{product,through:pos}`. On write/flush
failure the writer emits `retract{product,after:pos}` before following the existing
capture failure path. `after` is the last successful durable watermark, or null
if none exists in this run. Recovery segments never reuse published positions.
The ring retains provisional records and removes a retracted suffix. These
controls and records share the same ordered channel. A boundary can produce
`record(block 0), record(block 1), durable(through block 0), durable(through block 1)`:
a watermark covers only its prefix, not every record previously received.

**Consumers checkpoint only durable, fully applied positions.** Provisional
records may drive live display, but must be discardable. Persisted derived output
must also wait for durability or support rollback; a checkpoint alone does not
undo provisional output. On retract, discard all
provisional state after `after` (all of this run's provisional state if null),
pause application and resume from the journal using the last applied durable
checkpoint. Do not checkpoint a later recovery marker across an unresolved gap.
A watermark follows the configured flush/fsync policy: default `--fsync-blocks 1`
syncs every block before notification; weaker settings retain their documented
OS/power-loss exposure. No fanout protocol can strengthen that disk policy.

Client commands are UTF-8 JSON followed by LF (8 KiB maximum buffered input;
initial handshake deadline 5 s). One hello/resume per socket:

```json
{"type":"hello","version":1,"product":"BTC-USD"}
{"type":"resume","version":1,"product":"BTC-USD","pos":{"product":"BTC-USD","run_id":"UUID","block":123,"record":4}}
{"type":"resnapshot","product":"BTC-USD"}
```

`hello` also accepts `pos`. A position names the **last applied** record (normally the durable checkpoint
after reconnection): a hit
streams strictly after it, with no duplicate. A fresh hello without a position
streams the retained ring followed by live records; it does not promise a book
snapshot or full history. Resnapshot requires a subscription to that product.
It routes to `MarketDataFeeds::requestResnapshot(product)` and is limited to one
forwarded request per product per 20 seconds across all clients, matching the
engine's cooldown. A global rolling-window cap allows at most three forwards
per 60 seconds across products. Reply status is `forwarded`, `rate_limited`,
`global_rate_limited`, or `unavailable` during startup/shutdown. `forwarded` means
sent to the engine, which may still ignore it when disconnected/reconnecting;
it is not an acknowledgement of a new snapshot. Actual resnapshots are journaled
through the engine's existing path. Unknown products, malformed commands and repeat handshakes
close the connection with `protocol`.

Server packets have little-endian lengths, independent of TCP/socket reads:

```
u32 body_bytes | u32 json_bytes | JSON[json_bytes] | optional RAWL2 record
```

`body_bytes` counts everything after its own four bytes. JSON record headers
are `{"type":"record","pos":{...},"provisional":true}`; the remaining bytes are exactly the
RAWL2 `len | kind | systemNs | steadyNs | connection | payload` record, including
its four-byte length. Control packets have no raw suffix. JSON integer positions
are uint64 block / uint32 record; consumers must preserve integer precision.
No RAWL2 file header is sent: obtain product metadata by run ID from the journal.

Handshake replies:

- `tip`: `version:1`, `product`, `pos` (last fanout record, or null before any),
  and `durable` (last successful watermark, or null). This is a durability ceiling,
  not proof the consumer has applied that prefix; checkpoint only after replay
  and any journal gap have actually been applied. Replayed record headers remain
  provisional; the tip watermark qualifies their durability.
- `durable`: `product` and `through` (inclusive durable position).
- `retract`: `product` and `after` (last durable position, or null).
- Resume hit: `tip`, then every successor in the ring, then live records.
- Resume miss: `gap` with `reason:"resume_not_retained"`, the requested
  `resume_after`, `journal_until`, and `until_inclusive:false`; then `tip` and
  the ring/live records. Read the journal **after** the requested cursor and
  **before** `journal_until`, then apply the buffered socket stream. If the ring
  is empty, `journal_until` is null: the first subsequent socket record supplies
  that exclusive boundary. Tail the journal while waiting. Do not infer
  continuity from `tip` or silently skip an unavailable cursor/run. The exclusive
  boundary can still be provisional: journal EOF alone does not finish catch-up.
  Wait until the required prefix is durable/visible before joining socket replay.

The worker serializes replay and subscription registration, so there is no
replay-to-live race. Resume requires the cursor itself still in the ring;
expiration, unknown run, future cursor, and fanout ingress loss all give an
explicit miss. Ingress loss clears that product's ring and disconnects its
existing subscribers; the journal remains authoritative. Malformed ingress
similarly clears only the affected product's ring and disconnects its clients
with `malformed_ingress`, logging once per product while other products continue.
Capture writes never
wait on fanout locks or socket writes. Each client is serviced with a bounded
write budget; an overflowing client is disconnected without blocking others.

A best-effort `disconnect` packet carries `reason` and `resume:"journal"`.
It cannot be delivered reliably to an already full socket, and is never inserted
inside a partial record. **Every EOF requires discarding provisional state above the last fully applied
durable checkpoint and resuming from that checkpoint**, including EOF midway
through a packet. A full socket or ingress loss can also lose a retract/watermark;
absence of a retract is never evidence of durability. Disconnect
reason counters remain available on `/metrics`. Shutdown drains the disk
sessions first, makes one bounded send attempt and closes all clients; any
unsent suffix must be read from the journal. It never waits for a client to drain.

Probe (no exchange connection or journal writes):

```sh
python3 scripts/dev/fanout-tail.py BTC-USD
python3 scripts/dev/fanout-tail.py BTC-USD --resume '{"product":"BTC-USD","run_id":"UUID","block":123,"record":4}'
```

The probe prints positions and control messages, not raw payloads. Add
`--resnapshot` only for an intentional upstream resnapshot check; this affects
that product's capture. `--socket PATH` selects a fixture socket.

`CaptureFanoutTests` uses temporary roots and local sockets. It covers exact
writer bytes and positions across blocks/hour rotation, cursor-exclusive resume,
age/byte eviction, slow-client isolation, ingress loss, resnapshot isolation and
per-product/global rate limiting, safe paths/ownership, setup retry/backoff,
malformed-product isolation, idle clients and shutdown. It also
checks provisional delivery before flush, watermark ordering/replay, real short
write retraction, consumer rollback, null retraction before any durability,
position non-reuse, and Session failure before shutdown. The
application fixture additionally checks the default-enabled socket and metrics
with a client connected at SIGTERM, and journal continuity/recovery while an
unusable socket directory is repaired. No test uses production data or Coinbase.

Measurements (including append-to-receive p50/p95), validation and the deploy
watch list are in [Slice B as built](research/2026-10-one-world-pipeline.md#slice-b-as-built).
