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
JSON report. Exit 0 means no observed integrity failures in the supplied data;
2 means gaps, corruption, missing snapshot anchors, missing streams/segments or
other failed invariants, and 1 is a fatal invocation error. Existing files are
never repaired or rewritten by verification.

`ok` is prefix integrity, while `complete` additionally requires closed runs and,
for multi-product data, comparison against every destination stream.
`ok_closed_runs` states whether the closed runs passed. A run with its start but
no stop marker has `open: true` / `open_runs > 0`, and retains the legacy
`incomplete_runs` count. Its last file may have no index or a partial terminal
block/index without failing prefix integrity; complete CRC failures and interior
damage still fail. An open run awaiting its first snapshot is pending, not an
anchor failure, unless it already received unanchored updates. Closed connections
must have received that product's snapshot. A single middle hour generally fails
because the start/snapshot context is missing.

**Open does not prove the process is alive.** An interrupted or crashed process
without a stop marker is indistinguishable from a live writer in these files.
The report explicitly says it has checked only the readable prefix. Reverify
after close to certify completion. Files are scanned to their observed lengths;
whole-root digest comparison is deferred for open runs because product writers
flush at different times. Header creation itself is not an atomic read snapshot:
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
run, connection totals use the furthest observed product prefix. Totals' L2 and
disk counts sum the product reports; `totals.days` follows the same unique-frame
and physical-disk rules. Existing single-product top-level verification keys and
closed-run meanings remain; `products`, `totals` and status fields are additive.
Multi-product top-level counters mirror totals; detailed snapshot-size and p99
statistics remain in each product report.

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
gap marker in every product. On disk failure each stream independently attempts
its reserved failure marker in a new segment; an unwritable stream may reject it.
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
filtered. Acks, heartbeats, unknown channels, unclassified envelopes, JSON parse failures and
ambiguous/unknown product routing are copied to every product. No raw frame is
discarded. Each product stream has exactly one record per incoming frame: either
the raw kind-1 frame or a kind-9 routing receipt for a frame stored elsewhere.
This costs small compressed receipts for unrelated traffic but keeps replay in
one product directory and avoids decompressing unrelated large snapshots/updates.
A shared payload file plus side indexes would require coordinated index durability
and extra seeks; plain filtering would falsely imply connection sequence gaps.

V2 changes only the magic's last byte (`RAWL2\r\n\x02`), `format_version: 2`,
additional header fields, and permission for kind 9. Block, record, index and CRC
framing are identical to v1. Old readers reject v2 instead of silently losing
sequence proof. Single-product captures continue to write v1, with no new header
fields or record kinds. V2 headers retain the file's own `product_metadata` and
single-entry `products`; `connection_products` is the sorted, unique full
subscription set and `routing` is `"product-receipts-v1"`.

Kind 9 retains the original system/steady receive timestamps and connection ID.
Its UTF-8 JSON payload is a receipt with `products` (sorted raw-frame destinations),
`channel`, original `sequence_num`, `received_bytes` and the lowercase hex
`sha256` of the **exact** original frame bytes. It cannot name the current product
or a product outside the header's connection set. A receipt participates in global
sequence checks but never supplies a book snapshot/update. Replay still checks
all events belonging to the current product in each kind-1 L2 envelope, using that
product's exact metadata increments.

Whole-root verification requires every declared product stream for each run. For
closed runs it compares streaming SHA-256 identities without retaining frame
history: for each record, hash compact JSON array
`[kind, system_ns, steady_ns, connection_id, payload_string]` followed by LF.
For either frame form, normalize `kind` to 1 and `payload_string` to the compact
receipt JSON (keys sorted lexicographically, as nlohmann::json's default object);
for lifecycle markers use their exact payload string. For raw frames the receipt
is recomputed from the original bytes. Equal digests certify the same receive
order/clocks, content hashes, sizes, sequence identities and destinations across
all streams, despite independent block rotation. Each stream must also have the
correct raw/reference form for its product. Missing streams, changed duplicate
bytes/clocks, missing receipts and receipt/raw hash mismatches therefore fail.
These are integrity hashes, not authenticated signatures.

A product-only or single-file scan reports `scope: "product"` and
`routing_checks_deferred` for multi-product runs: it can prove sequence continuity
and that product's anchors, but cannot certify other products' raw payloads.
`connection_runs[].routing_checked` is true only after the whole closed stream set
has been compared. Reconstruct the exact connection by reading all streams for a
run, aligning records in stream order, and taking one raw copy wherever another
stream has a receipt; the root verification above checks those copies agree.

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
and missing streams or altered raw/reference bytes/clocks. Fault-injection checks
that disable subscription membership, routing, mixed-envelope handling, anchor
isolation, sequence checks, v1 versioning, unique accounting, open-prefix handling
or cross-file hashing each fail the corresponding regression.

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
