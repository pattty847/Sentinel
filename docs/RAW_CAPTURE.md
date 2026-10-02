# Pristine Coinbase capture

`sentinel-capture` opens its own Coinbase Advanced Trade WebSocket through
`MarketDataCoreEngine`. It subscribes to `level2`, `market_trades` and `heartbeats`.
One engine/transport subscribes all requested products on that connection.
It has no connection to sentinel-server, its recorder, the GUI or the local wire
protocol. It uses QtCore only. Future server rollups can consume these files;
this tool does not send raw L2 to clients.

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

For the owner's seven products on one connection:

```sh
./build/mac-clang/apps/sentinel-capture/sentinel-capture \
  --root /Volumes/T7/sentinel-data/raw-l2 \
  --symbols BTC-USD,ETH-USD,SOL-USD,FARTCOIN-USD,PEPE-USD,DOGE-USD,AVAX-USD
```

`--symbol` is repeatable; it can also be combined with `--symbols`. Inputs are
trimmed, validated, deduplicated and sorted. With neither option, the default is
BTC-USD. A single distinct product uses the unchanged RAWL2 v1 layout and bytes.
Up to 32 distinct products are accepted. Heartbeats remain connection-scoped
(the outgoing heartbeat subscription intentionally has no `product_ids`).

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
`~/Library/Logs/Sentinel/sentinel-capture-latest.log`, including a cumulative stats
line once a minute. `SENTINEL_LOG_DIR`, `SENTINEL_LOG_KEEP` and
`SENTINEL_LOG_STDERR` work as for sentinel-server. No secondary diagnostic log is
created. A disk error, queue overflow or oversized frame makes the process exit
nonzero with `Capture incomplete`. The queue reserves 4 KiB for a final stop/gap
record containing the reason and the first dropped frame's system/steady receive
times and connection ID. Accepted data drains before this marker on overflow.
After an I/O failure the damaged segment is left untouched and a fresh segment
is attempted for the marker. If the volume is still unwritable, even that marker
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
2 means gaps, corruption, interrupted older runs, missing snapshot anchors,
missing streams/segments or other failed invariants, and 1 is a fatal invocation error. Existing files are
never repaired or rewritten by verification.

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
anchor failure, unless it already received unanchored updates. Closed connections
must have received that product's snapshot. A single middle hour generally fails
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

Verification checks `market_trades` independently of WebSocket `sequence_num`,
without changing either RAWL2 format. The September 30 / October 1, 2026 archive
contains **descending** IDs inside both subscription snapshots and update
batches. The verifier sorts each event's selected-product trades by numeric
`trade_id` before checking continuity; JSON member order does not matter.
IDs must be unsigned 64-bit decimal strings. A product's state survives hourly
files and capture process runs; transport-up resets only its connection anchor.

* The subscribe `snapshot` is recent history, not replay since disconnect. Its
  IDs may overlap previously observed history. Internal snapshot holes are
  reported as `snapshot_trade_gaps` / `snapshot_missing_trades`, not integrity
  errors; these counters describe observed discontinuities, not remaining holes
  after possible later backfill.
* An update must continue the latest snapshot/update ID by one, after duplicate
  removal and numeric batch ordering. With no trade snapshot, the first update
  establishes the anchor; earlier coverage cannot be certified. The real archive
  also contains mid-connection snapshots followed by sparse old update batches.
  These snapshots contribute newly observed IDs and never rewind the anchor;
  old repeated IDs are duplicates. Once updates have begun, a snapshot cannot
  hide a hole ahead of the anchor: such a hole is a connected integrity error.
  `trade_snapshots` / `trade_resnapshots` count snapshots / those after updates.
  Empty and foreign-product events do not reset anchors.
* `within_connection_trade_gaps` counts gap intervals and
  `within_connection_missing_trades` sums missing IDs. Each is an integrity
  failure (`ok: false`, exit 2). `trade_gap_details` retains the first 30 with
  product, run UUID, connection ID, first missing ID, missing count, exchange
  trade time and receive system nanoseconds.
* The interval between the previous connection's highest ID and the first new
  ID after reconnect is counted separately in `reconnect_trade_gaps` and
  `reconnect_missing_trades`, with the first 30 `reconnect_trade_gap_details`.
  Overlapping snapshot history reduces or eliminates that interval. This also
  covers downtime between capture process runs. It does not fail file integrity:
  a clean capture can contain an incomplete trade tape because Coinbase did not
  replay disconnected trades. Missing counts describe each observed gap when
  encountered, not a later reconciliation ledger.
* `duplicate_trades` counts repeated IDs exactly, including reconnect snapshot
  overlap and duplicate updates. They do not fail verification or contribute
  again to totals. Seen IDs are compressed into intervals, capped at 65,536 per
  product; exceeding the cap fails explicitly instead of silently losing dedup
  accuracy. Ordinary contiguous data uses one interval, regardless of duration.

Each product reports `trades` (unique observed IDs, snapshots included),
`first_trade_time` / `last_trade_time` (UTC normalized to nanoseconds; null with
no trades), `buy_trades`, `sell_trades`, `buy_volume` and `sell_volume`.
Volumes are decimal strings in **base currency**, accumulated with 50-digit
precision; no binary floating point is used. Invalid IDs, times, sizes, sides,
event shapes or unexpected product IDs fail verification. Trade frames use the
routing SAX parser and retain only selected-product scalar fields for the
current bounded frame, with no full trade-frame DOM. L2 replay is unchanged.
Totals sum product counts; volumes stay per product because base currencies differ.

**Side correction for CVD:** Coinbase's [`MarketTrade` schema](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/advanced-trade-asyncapi.json)
defines wire `side` as the **maker's side**, not the aggressor's. The verifier's
buy/sell totals are **aggressor** totals: wire `SELL` adds buy volume, wire `BUY`
adds sell volume. Thus aggressor CVD is `buy_volume - sell_volume`; treating raw
`BUY` as an aggressive buy reverses its sign. `trade_side` states this mapping
in the report. The source bytes are never changed.

Zero connected-update gaps is evidence of contiguous IDs over the observed
connected intervals, not proof of full coverage before the first anchor, after
the last flushed frame, during downtime, or inside a sparse recent snapshot.
Check reconnect/snapshot missing counts and the existing open/interrupted/scope
status before treating the tape as complete.

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
| `--queue-mib` | `64` | Total connection disk queue budget including record overhead; 1..1024 MiB |
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
the bounded queue. One disk worker routes frames after queue admission, reuses
one zstd compression context per product and handles writes and sync. The queue holds each original
record once and has one total budget, not a separate allowance per product.
Overflow still stops the entire capture and attempts the same connection-wide
gap marker in every product. On disk failure only writers that threw abandon their damaged segment. Healthy
writers flush and seal their buffered data before appending a gap marker in a new
segment. A writer already closed successfully is left closed: a later peer's
close failure never adds a duplicate stop. Each remaining stream independently
attempts its failure marker; an unwritable stream may reject it.
Sync uses the shared persistence primitive: `F_FULLFSYNC` on Darwin with `fsync` fallback where full sync is unsupported. A record
is limited to 16 MiB (the existing Beast transport's default message limit), a
block to that record plus framing, and an index to 65,536 entries; a new segment
starts if that index limit is reached. Peak capture memory includes the queue,
one in-flight record and its routing parse, per-product raw/compressed block
buffers and indexes (bounded by 32 products), and the existing
engine's transport/JSON parser buffers. Writer failure or queue saturation ends
the capture with an explicit error. Fsync cannot recover bytes still in the
queue or current block; with defaults, block flush is targeted at one second or
1 MiB, plus disk scheduling delay.

The shared engine retries transport failures (including initial handshakes) with
1 s exponential backoff capped at 30 s, resetting on a successful connection.
Its heartbeat watchdog stays armed across failed retries; the existing 20 s
stale threshold and 5 s minimum stale-heartbeat backoff remain. This recovery fix
also applies to the server. A capture-only supervisor still recreates an engine
disconnected for 60 seconds as an independent safety net for a transport whose
connect/close callback never completes. The restart reason is recorded.

## Routing and RAWL2 v2 for multiple products

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
sequence proof. Single-product captures still write v1, with no new header fields
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
all product streams before the affected raw frame; engine invalidation/resync
markers remain connection-wide as well. This capture-only behavior does not
change the server's engine or recorder semantics.

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

The deterministic seven-product measurement uses five minutes of interleaved
BTC 22, ETH 10, SOL 5, FARTCOIN 2, PEPE 2, DOGE 1 and AVAX 1 frames/s, plus one
heartbeat/s and initial snapshots: 13,207 incoming frames. Each update contains
12 levels with deterministic varying quantities. Defaults are zstd level 3,
one-second/1 MiB blocks; fsync is disabled only for test speed. The baseline
rewrites the same per-product raw frames and lifecycle markers without receipts,
using the same header and block settings. Thus the delta includes compression,
extra framing and indexes, not just JSON sizes:

| Product | Own baseline bytes | With receipts | Overhead bytes | Overhead / own |
|---|---:|---:|---:|---:|
| BTC-USD | 723,605 | 725,121 | 1,516 | 0.21% |
| ETH-USD | 402,730 | 404,199 | 1,469 | 0.36% |
| SOL-USD | 262,758 | 264,136 | 1,378 | 0.52% |
| FARTCOIN-USD | 182,079 | 183,416 | 1,337 | 0.73% |
| PEPE-USD | 179,674 | 181,042 | 1,368 | 0.76% |
| DOGE-USD | 150,571 | 151,877 | 1,306 | 0.87% |
| AVAX-USD | 150,553 | 151,854 | 1,301 | 0.86% |
| Total | 2,051,970 | 2,061,645 | 9,675 | 0.47% |

That is about 2.8 MB/day of receipt overhead at this synthetic mix, with six
receipts per product. The regression asserts **under 5% for every product** and a
bounded receipt count. Compression sizes vary slightly with run UUID/header
values. This is a reproducible synthetic budget, not a promise about every live
payload distribution or a measured live-data rate.

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

Multi-product regressions additionally check the actual outgoing subscriptions
for all seven products through repeated/comma-separated/mixed CLI forms; exact
routing and receive clocks; a mixed-product L2 envelope; independent metadata
increments and snapshots on reconnect; connection-wide gaps/invalidations;
shared-budget overflow; unique totals and daily accounting; v1 layout/report
compatibility; active open-prefix verification; mixed v1/v2 runs and hour rotation;
and missing streams or altered raw/reference bytes/clocks. Review regressions add
frozen routing/digest vectors, snapshot allocation bounds, superseded interrupted
runs, crash-tail raw loss, isolated writer failures, shared queue saturation with
individually fitting frames, live exit 3 and event-driven application readiness.
Trade regressions cover descending contiguous batches, connected holes,
reconnect loss, exact snapshot/update deduplication, aggressor volumes and time
precision, product isolation in mixed envelopes, v1/v2, hourly/process boundaries,
invalid scalars and overflowing IDs, and real mid-connection snapshots followed
by sparse historical updates. A mid-connection snapshot cannot hide a forward
hole. Targeted mutation checks disable these behaviors and must fail their
regressions; every restored source is touched and rebuilt before proceeding.

## launchd arguments (review/deploy separately)

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

The 64 MiB queue is shared by all seven products. Overflow retains the existing
nonzero exit/restart contract; queue size is a memory budget, not a loss guarantee.
No exchange credentials are required for these public channels.
