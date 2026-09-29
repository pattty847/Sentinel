# Pristine Coinbase capture

`sentinel-capture` opens its own Coinbase Advanced Trade WebSocket through
`MarketDataCoreEngine`. It subscribes to `level2`, `market_trades` and `heartbeats`.
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

After stopping, verify the whole product directory to preserve the snapshot and
sequence context across hour boundaries:

```sh
./build/mac-clang/apps/sentinel-capture/sentinel-capture \
  --verify /Volumes/T7/sentinel-data/raw-l2/BTC-USD
```

`--verify` also accepts one `.rawl2` file. It is offline and read-only; stdout is a
JSON report. Exit 0 means all supplied runs were complete and verified, 2 means
the report found incomplete data, gaps or failed invariants, and 1 is a fatal
invocation/I/O error. A single middle hour usually lacks the original snapshot
and start/stop markers: it can pass block integrity while remaining incomplete.
An active capture likewise has no final index/stop marker yet. Existing files are
never repaired or rewritten by verification.

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

## Configuration and limits

The capture has CLI options only; it does not load or modify server/client YAML.

| Option | Default | Meaning |
|---|---|---|
| `--root` | `/Volumes/T7/sentinel-data/raw-l2` | Absolute storage root; must resolve to a mounted volume |
| `--symbol` | `BTC-USD` | Single Coinbase product |
| `--block-ms` | `1000` | Block age target, measured from receive steady-clock time; 1..60000 ms |
| `--block-bytes` | `1048576` | Uncompressed byte target; an individual frame remains whole |
| `--fsync-blocks` | `1` | Sync every N blocks; 0 syncs at file close only |
| `--zstd-level` | `3` | 1..19 |
| `--queue-mib` | `64` | Pending disk queue budget including record overhead; 1..1024 MiB |
| `--duration` | `0` | Seconds until clean stop; 0 waits for a signal |
| `--key-file` | `key.json` | Existing optional Coinbase credentials |
| `--jwt` | off | Enable existing engine JWT auth; public channels need no credentials |
| `--ca-bundle` | `resources/certs/ca-bundle.crt` | Existing REST/WS TLS CA bundle |

The root never falls back to another disk when `/Volumes/<name>` is absent.
The server's `/Volumes/T7/sentinel-data/recording` tree is refused, including
canonicalized symlinks. One process holds a per-product lock. Metadata is fetched
first using `CoinbaseRestClient`, retaining the returned `quote_increment` and
`base_increment` strings and the complete product JSON. Metadata failure prevents
startup; no WebSocket is opened with guessed increments. Credentials/JWTs are
never placed in the capture header.

The ingest observer is called before parsing and only stamps/copies records into
the bounded queue. A disk worker reuses one zstd compression context and handles
writes and sync. Sync uses the shared persistence primitive: `F_FULLFSYNC` on
Darwin with `fsync` fallback where full sync is unsupported. A record
is limited to 16 MiB (the existing Beast transport's default message limit), a
block to that record plus framing, and an index to 65,536 entries; a new segment
starts if that index limit is reached. Peak capture memory includes the queue,
one in-flight record, raw/compressed block buffers, the index, and the existing
engine's transport/JSON parser buffers. Writer failure or queue saturation ends
the capture with an explicit error. Fsync cannot recover bytes still in the
queue or current block; with defaults, block flush is targeted at one second or
1 MiB, plus disk scheduling delay.

The shared engine retains its sequence checking and heartbeat resync. A
capture-only supervisor recreates an engine disconnected for 60 seconds, covering
initial-handshake failures before the shared heartbeat watchdog starts. The
restart reason is recorded. No server reconnect behavior changes.

## RAWL2 v1 format

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
verification. Valid prefix blocks remain recoverable. Missing indexes or start/
stop markers are reported as incomplete, including a crash exactly between blocks.
The verifier bounds file discovery at 100,000 files and book replay at 2,000,000
levels. Book prices/quantities use checked integer arithmetic with metadata
increments (up to 18 decimal places, 64-bit normalized mantissas/atoms), never
floating point. Off-grid, negative, malformed or overflowing values fail replay;
the original raw bytes remain preserved. Invalidations clear the reconstructed
book until another accepted snapshot; updates during an unanchored interval are
reported rather than treated as observed liquidity.
