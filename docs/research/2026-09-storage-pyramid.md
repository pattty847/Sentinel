# Order-book storage pyramid: design and measurement

Research date: 2026-09-29. Branch: `lt-astra/storage-research`.
Initial scope: isolated probe and offline tests; no recorder, server, GUI, or wire behavior changes.
The owner-authorized crash follow-up also fixes the shared stream client's write
serialization and transport teardown; recorder and server behavior remain unchanged.

The useful storage object is **linear liquidity plus its observation semantics**, from
which the client derives paint intensity. Keep bids and asks separate until presentation.
A sparse time pyramid is a good fit, but the proposed exactness and bounded-work claims
need conditions. Do not select 100 ms retention from the current ~15 MB/day minute
measurement by multiplying by 600; change frequency, temporal deltas, headers,
keyframe cadence, sparsity and peak retention change the ratio.

## Owner decisions (2026-09-29)

These decisions override the rest of this document where they conflict. The heatmap side is in
`2026-09-gpu-heatmap-integration-plan.md`, section "Owner decisions (2026-09-29)".

**Direction**
- Raw pristine accepted L2 is the target durable source of truth. This settles the "two
  products" question below: full fine-detail book history must be recoverable. The paint
  pyramid is derived from raw.
- Never throw away finer book detail than the UI shows. Store the finest useful source data.
  What a user may see (entitlement) is a separate question.
- Raw L2 stays sparse: snapshots, absolute quantity updates, zero deletes, and
  gap/reset/validity markers. No dense price arrays.
- Server serving levels: raw L2 -> 1s / 1m / 1h rollups -> immutable chunks -> client.
  Sub-second levels (including 100 ms) come later. Deleting fine source data must never be a
  requirement of the design.
- Storage is not scarce, but compress properly: sparse absolute updates, timestamp deltas,
  integer tick prices, varints, periodic keyframes, independently decodable blocks, zstd,
  bounded reconstruction chains, random-access indexes.
- **100 ms retention is undecided** until the pristine capture below is measured. Do not
  finalize any capacity assumption before it.

**The 60-minute probe is not sufficient.** It measured the coalesced local stream downstream of
`SentinelStreamServer` (band-clipped `liveBook`, `BookDelta` with `float` quantity, no upstream
sequence, dequeue time). See "Critical local-feed limitation" below. Its 169 MB/day raw figure
is a lower bound, not a capacity number.

**Next measurement: capture at the true Coinbase ingest seam.**

The seam is the `m_transport->onMessage` lambda in the `MarketDataCoreEngine` constructor
(`libs/core/marketdata/MarketDataCoreEngine.cpp`). It receives each Coinbase WebSocket text
frame as a `std::string`, before `nlohmann::json::parse` and `dispatch()`. The capture also
needs two engine events that are not frames: transport up/down (`m_transport->onStatus`) and
`emitBookInvalidated`. Adding this tap changes server code, so it is its own authorized slice.

What exists at each layer:

| Field | Raw frame at the seam | Engine callbacks (`onLiveOrderBook*`; what `BookRecorder` receives) | Local stream (the probe) |
|---|---|---|---|
| Envelope `sequence_num` | Yes. Per connection, contiguous across **all** channels (`l2_data`, `market_trades`, `heartbeats`, acks), starts at 0 | Checked in `dispatch()`; not forwarded | No |
| Envelope `timestamp` | Exact ISO 8601 text | `Cpp20Utils::parseISO8601` keeps microseconds; callbacks pass ms | No (dequeue time) |
| Per-update `event_time` | Yes (snapshot entries carry `1970-01-01T00:00:00Z`, per `tests/marketdata/fixtures/coinbase_messages.hpp`) | Ignored | No |
| `price_level`, `new_quantity` | Exact decimal strings | `double` via `fastStringToDouble` | Indexed price, `float` quantity |
| Snapshot vs update | `events[].type` | `onLiveOrderBookInitialized` / `onLiveOrderBookLevelUpdates` | One snapshot, then deltas |
| Zero-quantity snapshot entries | Present | Dropped | Dropped |
| Validity and resync | Derivable from sequence and frames | `onLiveOrderBookInvalidated(product, reason)`: `disconnected`, `sequence gap expected=N got=M`, `malformed snapshot entries=N`, `malformed update level` | None |
| Local receive time | The capture must stamp it (system and steady clock) | Not forwarded | Dequeue time |

Reset semantics: a sequence gap, a malformed level or a stale heartbeat makes the engine close
the transport and reconnect. A new connection restarts `sequence_num` at 0 and resubscribes,
and Coinbase sends a fresh `snapshot`. A transport down emits `disconnected` for every product.
The book is valid again only at the next snapshot for that product.

Precision: Sentinel does not fetch the product's `quote_increment` or `base_increment` today.
Integer tick prices and integer quantity atoms need them, so the capture records them (from
Coinbase product metadata) or proves them from the decimal strings.

Capture contract:
1. Record every frame on the connection, all channels. The sequence is shared, so an
   `l2_data`-only capture cannot prove completeness.
2. Store the exact frame bytes, local system-clock and steady-clock receive times, and the
   up/down and invalidation events, in length-prefixed zstd blocks.
3. Run at least 24 hours of BTC-USD, including a volatile period. Record the subscribed
   products and the connection count.
4. Report frames per second (mean and p99), bytes per day as received, gaps, reconnects and
   snapshot sizes.

Implementation: `sentinel-capture` is a standalone QtCore process with its own
Coinbase engine/connection. It writes pristine messages and transport/validity
markers to append-only hourly RAWL2 segments under
`/Volumes/T7/sentinel-data/raw-l2/BTC-USD`, with independent zstd blocks, CRCs and a
rebuildable index. It never uses the running server or its recording directory.
See [capture start/stop/verify commands and format](../RAW_CAPTURE.md). The offline
fixture tests establish the format/replay contract; the owner/orchestrator must
still run and verify the live 24-hour measurement before drawing capacity conclusions.

**Decided (owner, 2026-09-29 PM): raw deltas + periodic keyframes (option A) is the storage
method.** Per-second snapshots lose everything inside a second, so they cannot meet "full
fine-detail history recoverable". Option B is still measured below, only as a reference point
for the owner's learning and for keyframe spacing. The pristine capture is approved; the owner
has about 800 GB on the T7 for it and will add drives as needed, and wants to learn the
compression side hands-on (a compression lab: a bench tool that replays captured raw data
through candidate encodings and prints bytes and speed).

**Benchmark: raw deltas + periodic keyframes vs full-book snapshots every second.** Replay the
capture offline through both encoders:
- **A.** Canonical events (integer ticks, integer quantity atoms, varint timestamp and sequence
  deltas, side bits, validity markers), plus a full-book keyframe every K seconds or bytes.
  Independently decodable blocks with a bounded chain.
- **B.** An explicit full-book snapshot every second, with the same integer and zstd techniques.
- **Baseline.** zstd of the exact frame text (upper bound, no parse loss).

Judge them on:
- full fine-detail book recoverability: the exact book at any event time, including
  sub-second changes, peaks and exact TWAP. B cannot recover changes inside a second by
  construction, so state what it loses;
- bytes per day;
- encode and decode CPU;
- random-access cost: the time and bytes to rebuild the book at an arbitrary time, and to
  build one 1s, 1m and 1h column.

Pick whichever reaches the fidelity target most efficiently. A hybrid (deltas + frequent
keyframes) is allowed.

Also evaluate a numeric column codec in the style of pcodec for A, B and the serving chunks.
Prior art (`2026-09-marketlens-har.md`, observed): MarketLens ships each 64-column order-book
chunk as one pcodec file, 16-237 KB per market-chunk, and zstd on top adds almost nothing. If
the chosen codec is already entropy-coded, do not add zstd over it. Whether MarketLens stores
dense grids or sparse levels inside pcodec is not observed.

**Terminology correction.** About 0.0423% is the **half-code relative quantization error** of
the 15-bit log magnitude encoding (`2^(1/(2*819))-1`). One code step is about 0.085%. It is
not a cumulative drift. The real limit: HMC2 stores a quantized **mean**, not the time integral,
so decode -> aggregate -> re-encode cannot be exact. The future mergeable representation is
A (quantity x time integral), D (observed duration or coverage) and P (peak), with explicit
validity and coverage. That is the next storage generation. **It does not block the current
GPU and chunk integration.**

## Recommendation and decisions still open

Prototype a 100 ms / 1 s / 1 m / 1 h pyramid of mergeable aggregate state, with a
byte-budgeted query planner and independent time/price display resolution. Treat
100 ms and raw retention as measured product choices. Keep the existing minute
recorder in service while these choices are evaluated. The probe also measures
10 s as a possible intermediate retention tier; it does not measure a new production
pyramid or a sum-based format.

Use server-side time composition when source volume exceeds the client's cache
budget; retain client/GPU price binning from a bounded sparse source page for cheap
pan/zoom. Sending only precolored pixels loses reversible thresholds, quantities,
both sides, and coverage. Sending every fine sparse column to the browser merely
moves the amplification problem to transport and the GPU. Cache immutable source
chunks on both sides, then choose where composition costs least.

Decide explicitly between two products (decided 2026-09-29: raw replay-grade source of
truth, paint pyramid derived; see "Owner decisions"):

- **Paint history:** time means, temporal peaks, valid zero/missing distinction,
  with declared quantity quantization. A pyramid can be the durable archive.
- **Replay/research history:** accepted source snapshots, absolute-quantity updates,
  sequence/reset/invalid markers and clocks, plus a derived pyramid. L2 supports
  book-state replay but does not identify queue positions, individual orders, or
  which cancellation/trade caused a size decrease. Trade tape remains separate.

Raw capture is optional for the first product, necessary to rebuild arbitrary
resolutions/policies after throwing fine aggregates away for the second. A cache
is rebuildable only while its actual source survives.

## Exact composition and its limits

For a fixed asset, native price row `r`, side `s`, and half-open interval `I`, let
`q(t)` be the aggregate absolute resting quantity and `v(t)` indicate that this
row/side is known under the recording window policy. Store:

```
A(I,r,s) = integral_I q(t) * v(t) dt    [quantity * milliseconds]
D(I,r,s) = integral_I v(t) dt          [milliseconds]
P(I,r,s) = max observed q(t)           [quantity; separate empty/unknown flag]
TWAP = A / D when D > 0; otherwise unknown
```

For nonoverlapping intervals on the **same grid, quantity scale, validity and
window policy**, merge `(A,D,P)` with `(sum, sum, max)`. It is not an arithmetic
average of averages: 10 units over 10 ms and 0 over 90 ms means 1, not 5.
A valid zero contributes duration even when it has no sparse quantity entry.
A disconnect contributes neither numerator nor duration. Repeated revisions
must replace the same bucket, never add it a second time.

This algebra is exact over specified timestamps and input quantities. Machine
arithmetic needs its own contract. Fixed-point quantity atoms times integer ms,
with a checked wide accumulator (potentially 128-bit), can preserve exact integer
sums. Float64 sums are approximately associative; deterministic reduction order
and explicit tolerances are needed. Choose quantity scale per instrument/version,
not per viewport, and prove multiplication/accumulation bounds before narrowing.

The current `RecordingCodec.hpp` encodes a **mean**, not `A`: 15-bit logarithmic
magnitudes at 819 codes/octave, default floor 1e-6, plus side. Away from floor and
saturation, half-code relative rounding is about `2^(1/(2*819))-1 = 0.0423%`.
Decoding a mean and multiplying by duration yields a quantized estimate of `A`.
Re-encoding at every level compounds rounding. Exact sums therefore require a
new versioned representation, or retaining unquantized state alongside paint codes.
Do not label existing HMC2 rollups exact exchange-size integrals.

There are two further noncommuting operations:

1. **Window selection before time aggregation.** `BookRecorder` integrates all
   book rows in RAM, then crops a finished column to its mid-min/mid-max envelope
   (near: lower row edge in `[.95*midMin,1.05*midMax]`; deep: `[.25*midMin,4*midMax]`).
   A direct 1-minute column can include a wall held early outside a 100 ms window
   because the mid moves toward it later. That early mass is absent from stored
   fine columns and cannot be recovered by summing them. Define the new pyramid
   as rollups of the finest recorded validity/window policy, use a stable wider
   canonical grid, or retain raw/full row state. Independent direct coarse
   recordings can preserve today's envelope behavior but are not exact rollups
   of a cropped base. Near $1 detail cannot be reconstructed from deep $5 rows.
2. **Time peaks after price binning.** `max_t(sum_rows q)` is not
   `sum_rows(max_t q)`. Two rows each peaking at 10 at different times may have a
   bin peak of 10 while their peak sum is 20. Native-row peak composes over time;
   it does not recover a larger price bin's exact simultaneous peak. Offer a
   strongest-native-row peak, label summed peaks as an upper bound, or retain
   the finer time/event state needed for that different statistic.

## Per-level book state versus sums

The ingestion process needs a current price-level book, an accepted snapshot,
and a gap/reset state machine regardless of the archive. Updates are absolute
quantities, with zero deleting a level. Apply a received batch atomically before
updating peaks; sequential within-batch peaks can invent liquidity.

One shared book and lazy per-row integrals avoid scanning the full book per
message. Close base intervals at epoch boundaries even during silence; silence
is not a gap if the source connection remains valid. Roll coarser state from
closed base sums rather than running separate full books per level. A few forming
coarse accumulators are small compared with keeping every requested timeframe.
The probe intentionally maintains four independent books to compare individual
encoder costs; summing its CPU numbers is not the cost of this proposed shared
implementation.

Storing only endpoint snapshots at every level is simpler but misses intrabucket
changes and cannot answer TWAP or peaks. Storing a mean **with exact duration** is
algebraically equivalent to sums only before quantization and rounding. Storing
sums/coverage directly avoids that ambiguity. Prefix sums could accelerate range
means but require large cumulative values, revision handling and a separate peak
index; a hierarchical range decomposition is simpler for sparse mutable edges.

## Time alignment and read amplification

All buckets are UTC epoch intervals `[floor(t/T)*T, floor(t/T)*T+T)`. For `T=22 s`,
boundaries continue every 22 seconds from the Unix epoch; they do not restart at
every minute, hour, or day (86,400 is not divisible by 22). Query/chunk/day boundaries
must not reset the aggregation phase. A base `B=100 ms` can exactly cover all these
boundaries for integer multiples of B. An arbitrary 125 ms period or a cut through
a base bucket needs finer data, raw integration, or an explicitly approximate rule.
Do not scale a partial base sum by overlap and call it exact.

Let `N <= 4000` be output columns, `L(T)` the largest available stored interval
that divides T with compatible epoch alignment, and `E_L` the mean visited sparse
entries/coverage runs per source column. The single-level plan costs:

```
A_time(T) = T / L(T)
source_columns = N * A_time(T)
entry_work ~= N * A_time(T) * E_L + decoding warmup + output work
bytes_read = sum(compressed frames actually fetched, including warmup/metadata)
```

For levels {100 ms, 1 s, 1 m, 1 h} and complete retention:

| Requested T | Chosen L | Columns read / output | For N=4000 | Time visible at N=4000 |
|---|---:|---:|---:|---:|
| 100 ms | 100 ms | 1 | 4,000 | 400 s |
| 1 s | 1 s | 1 | 4,000 | 66 m 40 s |
| 10 s | 1 s | 10 | 40,000 | 11 h 6 m 40 s |
| 22 s | 1 s | 22 | 88,000 | 24 h 26 m 40 s |
| 1 m | 1 m | 1 | 4,000 | 66 h 40 m |
| 16 m | 1 m | 16 | 64,000 | 44 d 10 h 40 m |
| 4 h | 1 h | 4 | 16,000 | 666 d 16 h |
| 3,601 s | 1 s | 3,601 | 14,404,000 | ~166.7 d |

The proposed fixed-ratio bound is **false for arbitrary requested periods** with
the single-divisor plan. A large prime number of seconds cannot use minutes/hours;
even hour multiples grow unbounded above the highest retained level. Restrict
supported periods, extend levels upward, or use a mixed-level interval planner.

With nested levels `L0=B < ... < Lk`, integral ratios `ri=L(i+1)/Li`, cover each
output interval with the coarsest aligned interior blocks and recursively cover
its two edges. A conservative count bound per output is:

```
C(T) <= floor(T/Lk) + 2 * sum(i=0..k-1)(ri - 1)
```

For this pyramid the edge bound is `2*(9+59+59)=254`, independent of divisibility.
Aligned cases are much cheaper. The `floor(T/Lk)` term still requires larger
levels or a maximum requested T for an absolute bound. Adjacent output intervals
can share fetched chunks/cache pages, but their sums must never overlap. A binary
pyramid gives `O(log(T/B))` range blocks with more stored levels; minute/hour
ratios favor familiar periods at the cost of a larger edge bound.

These are **column counts**, not latency guarantees. Sparse coarse columns may
contain the union of many changing rows. Add explicit compressed-byte, decoded
entry, coverage-run, CPU and GPU upload budgets. A 4,000-column display alone does
not bound row count, cold I/O, or delta-chain reconstruction.

The existing 15-minute keyframe period implies up to 9,000 columns/chain at 100 ms,
900 at 1 s, 90 at 10 s, and 15 at 1 m. A cold random 100 ms read can reconstruct far
more history than the view needs. The probe preserves that period for comparison;
a production fine-level chunk should instead cap chain **column count and bytes**.
Independent 128-256-column blocks are a starting benchmark, not a measured optimum.

## Coverage and price grids

Current schema-4 hours carry per-entry covered milliseconds and disjoint per-side
coverage runs for zero rows. `RecordingPage.cpp` uses native row denominators and
preserves unknown regions. Keep that distinction at every proposed pyramid level.
A global `observedMs` and bounding envelope cannot represent row holes from a moving
window. Sparse absence means zero only where coverage proves observation.

On time merge, sum native-row durations and numerator; merge zero-row coverage
with a sweep of interval endpoints, retaining disjoint runs and counts. Retain
source flags (partial, resynced, approximate legacy coverage), grid/config identity,
and the interval known to have been scanned. No-record may mean a known gap or
an unscanned range; those must remain distinct in pagination.

On price merge, do not divide the sum of all row numerators by the **sum of row
durations**: with common duration D that would average price rows instead of
summing liquidity. With equal support use `(sum_rows A)/D`. With unequal support,
`sum_rows(A_r/D_r)` is a sum of rowwise observed means, not an observed mean of the
simultaneous full band. Choose that labeled statistic or require common complete
support. Duration counts alone cannot recover intersections of different temporal
gaps. Conservative per-band validity avoids asserting a complete book where one
constituent is unknown. Never sum log intensities or split $5 totals into fabricated
$1 rows. Across native-tick generations use compatible integer multiples or mark
incompatible portions unknown.

## Serving chunks and the live edge

Candidate immutable chunk key: `(venue, symbol, layer, native-grid/config-version,
level, [start,end), generation/content-hash)`. Metadata includes timestamp origin,
quantity/time units, encoding version, row bounds, coverage/flags, checksums,
first/last committed bucket and a bounded frame index. Symbol alone is insufficient
if multiple venues or instruments with different unit conventions are introduced.

Start measurements with independently decodable blocks of at most 128-256 columns
and a target 256 KiB-1 MiB compressed, with a hard decoded-size cap. Allow smaller
blocks for dense deep books; measure actual compression and request overhead.
Group blocks into larger immutable files/objects with a footer index and range
reads, rather than one object per column. Optional native-price slabs let the server
skip offscreen depth without decompressing a full book; slab/column choices need
real request traces. Neither those chunk targets nor transport latency are measured
by the current probe, which uses one zstd frame per column like HMC2.

The client receives native linear quantities/sums, coverage, side and stable grid
metadata. It may cache source chunks and form requested time buckets locally within
budgets. The server can return/cache requested-T sparse columns when fine-source
amplification is high. GPU price gather/binning then produces the chosen statistic,
followed by nonlinear intensity mapping. Timeframe is the column width regardless
of zoom; a 1-pixel minimum caps **display** columns, not server read work.

Keep an immutable committed prefix and revisioned forming suffix. Publish
`bucketStart`, source generation, revision, observed-through time, covered duration,
and a commit watermark. Replace a provisional, finalize once, ignore older
revisions; never repeatedly add cumulative live snapshots into rollups. A coarse
forming bucket combines committed child sums plus one replacement snapshot of
uncommitted children. When a watermark passes its exclusive end it becomes final.

Use event timestamps with a specified lateness policy for a production archive;
record receipt clocks separately. On sequence gap/disconnect/malformed L2, end
validity and require a fresh snapshot. Do not extend the last book through an
outage or use a wall-clock timer as proof of upstream health. Resume reconnecting
clients from a known watermark; request missed finals by time, with bounded live
and history work queues. This follows the existing S4 immutable-publication model
without changing it in this task.

## Retention, compaction and rebuilds

Example policy for costing, **not adopted defaults**: raw and 100 ms for 7 days,
1 s for 30 days, 1 m for 365 days, 1 h indefinitely. Price detail is a separate axis:
keeping only deep hours removes near $1 detail regardless of temporal resolution.
At older ages a 22 s request is unavailable if seconds/raw have expired; a 16 m
request is unavailable if only hours survive. Advertise per-layer/time/grid
availability and do not silently paint an upsampled hour as exact fine history.

For daily byte rates `R_i`, finite retentions `d_i`, and archive age `Y` days:

```
retained_bytes(Y) ~= sum_i R_i * min(d_i,Y) + R_forever * Y
                  + indexes + replicas + compaction working space
```

Rates differ per level and may change with market activity. A temporal pyramid is
not necessarily the geometric size sum of its bucket counts: union sparsity,
coverage metadata and exact numerators can dominate. Disk rates also exclude
replication, filesystem allocation and read caches.

A single owner compacts each immutable stream: freeze a source manifest/generation,
merge complete nonoverlapping children, write/checksum a new object, verify counts,
coverage and boundary sums, sync and atomically publish the manifest, then tombstone
inputs and garbage-collect after a reader grace period. Crash recovery reuses or
removes orphan output; never delete source before a verified replacement is
published. Keep retention behind the completed compaction watermark and its margin.
Late corrections create a replacement generation and invalidate dependent rollups.
Deletion of base data permanently removes the ability to change past aggregation,
validity or near/deep window policy unless raw source is retained.

## Probe usage and measurement contract

Build and offline verification from the repository root:

```sh
cmake --build --preset mac-clang --target storage_probe test_storage_probe
ctest --test-dir build/mac-clang -R '^StorageProbeTests$' --output-on-failure
build/mac-clang/tests/servermodel/storage_probe --synthetic --minutes 10 --out /tmp/storage-synthetic.json
```

Live run for the orchestrator, against an already warm local server:

```sh
build/mac-clang/tests/servermodel/storage_probe --minutes 30 --out /tmp/storage-btc-30m.json
# If this worktree lacks the existing server certificate:
build/mac-clang/tests/servermodel/storage_probe --minutes 30 --ca /path/to/certs/sentinel-server.crt --out /tmp/storage-btc-30m.json
```

Do not generate a different certificate for the running server. The default endpoint
is `wss://127.0.0.1:8080`; certificate parsing is checked before constructing the client
so its development fallback cannot silently disable verification. The probe subscribes
to BTC-USD, starts timing on its first two-sided snapshot, uses queued Qt delivery,
and stops with a failed JSON result on connection error/disconnect. No snapshot within
15 seconds is a failure, not a zero-byte measurement. No GUI is created.

All five simulations consume the same sample interval, interleaved on one CLI thread;
this is a simultaneous comparison, not a multi-core throughput benchmark. Encoder CPU
uses thread CPU time around book application, integration, serialization, CRC and
zstd; it excludes network, JSON parsing, event-loop waiting and output I/O. Each TWAP
encoder maintains its own book and near/deep row sums. Real shared ingestion/pyramid
CPU must be measured separately. `max_timer_delay_ms` exposes sampling-loop stalls;
large stalls make dequeue-time 100 ms TWAP comparisons unreliable.

- Raw blocks: snapshot/delta/invalidation message boundaries and order; timestamp
  delta varints, lossless binary64 XOR varints for parsed prices/absolute sizes,
  side byte, zstd level 3. Flush at the next event after 1 s or 1 MiB of raw block
  data, plus final flush. One message is atomic (500,000-level cap); the block hard
  limit is 16 MiB. Count a modeled 32-byte stream header and actual 16-byte framed
  compressed blocks. Blocks decode independently; **book replay** still needs the
  preceding snapshot. There is no periodic full-book replay checkpoint in this
  measurement, so a replay service with more checkpoints will cost more.
- Columns: 100/1,000/10,000/60,000 ms epoch buckets, near $1 +/-5%, deep $5
  [mid/4,4*mid], matching the recorder's finished-column envelope policy. Lazy
  duration integrals, observed-ms denominator, optional peak (`--no-peak` writes
  zero in the existing peak field), HMC2 schema-3-style temporal deltas, absolute
  keyframes at UTC 15-minute boundaries or after gaps/day changes/peak-only rows,
  zstd level 3 per column, real CRC/framing, modeled daily HMC2 file headers.
  Initial/final partial columns are included and counted. A no-peak run tests
  compression with zero peak fields, not a redesigned format that removes them.
- The result is **encoded-byte simulation**, not a disk-throughput/fsync test. Frames
  are compressed and discarded after accounting; only JSON is written. No index,
  allocation-unit, compaction, periodic snapshot, network or replication overhead
  is implied. The codec mirror is byte-parity-tested against actual `Hmc2Store`
  minute files, including removals, peak-only keyframes and day rollover.

For measured interval `S` seconds, reported rates are `bytes*3600/S` and
`bytes*86400/S`; byte units are decimal, not MiB. Both per-layer and combined TWAP
counts are available. Record observation fraction, partial columns, messages,
levels and invalidations along with rates. Startup snapshot/keyframes are included;
repeat longer intervals, including volatility, and compare rates before planning
capacity. The unit tests verify lossless raw round trips/CRC rejection, all four
TWAP resolutions, atomic peaks, side separation, gaps/resync, moving windows,
quiet-book advancement, epoch/partial edges, malformed input, production minute
integration parity and physical file-size/byte parity.

### Critical local-feed limitation

`SentinelStreamClient::l2UpdateReceived` exposes parsed `BookLevelUpdate` vectors;
`liveOrderBookUpdated` is not the receive path to use. It discards no additional
quantities in this probe. However, `SentinelStreamServer` constructs its snapshot
from the indexed `liveBook` and sends updates from `BookDelta` (`float qty`, indexed
price converted back to a price). This is already a bounded/grid-converted feed,
not the original full-depth Coinbase book. The local messages/signals omit upstream
sequence, exchange timestamp and explicit upstream book-invalid/resnapshot status.
`MarketDataCoreEngine` itself detects sequence gaps and requests resynchronization,
but the probe cannot infer those intervals from this interface.

Consequently the report says `upstream_sequence_and_validity_available=false` and
uses monotonic elapsed time anchored to first-snapshot **dequeue** UTC. A silent
upstream outage can look like a quiet valid book. The raw encoder is lossless only
with respect to parsed local events, not exchange JSON, decimal originals or event
time. These measurements cannot establish full-depth raw/archive cost, deep-layer
completeness, production exactness, or backtest-quality validity. A follow-up full
source capture at the accepted Coinbase ingest seam is necessary before choosing
raw storage or comparing with the owner's production ~15 MB/day number. Doing that
would change the task's permitted production scope, so this probe exposes the
limitation rather than altering the server.

### Measurement table

Live run: BTC-USD, 60 minutes, 2026-09-29 04:04-05:04 EDT, local parsed stream from
the production recorder server (`storage_probe` at `47da96f`), zstd level 3, near and
deep layers per the production config, 15-minute keyframes, peak enabled. 65,618 stream
messages (about 18/s; the server coalesces upstream deltas), one snapshot, no
invalidations. The production reference and local-feed minute row do not have the same
input-depth/precision contract.

| Source / encoder | Duration | Encoded bytes/hour | Extrapolated MB/day | CPU seconds (1 h) | Notes |
|---|---:|---:|---:|---:|---|
| Existing production minute recording | owner measurement | ~625,000 | ~15 | not supplied | Reference only |
| Local raw parsed L2 | 60 min | 7,042,508 | 169.0 | 0.34 | Includes initial snapshot; lower bound for full exchange L2 |
| Local 100 ms TWAP + peak, near+deep | 60 min | 21,764,868 | 522.4 | 49.7 | near 12.1 MB/h, deep 9.6 MB/h |
| Local 1 s TWAP + peak, near+deep | 60 min | 6,123,905 | 147.0 | 5.5 | near 4.2 MB/h, deep 2.0 MB/h |
| Local 10 s TWAP + peak, near+deep | 60 min | 1,862,972 | 44.7 | 1.8 | Candidate intermediate tier |
| Local 1 m TWAP + peak, near+deep | 60 min | 796,326 | 19.1 | 1.4 | Includes startup keyframes; production runs ~15 MB/day |
| Exact-sums pyramid | not implemented | not measured | not measured | not measured | Requires new schema and common validity/window policy |

Reading (orchestrator, 2026-09-29): per year that is about 62 GB raw, 190 GB at 100 ms,
54 GB at 1 s, 16 GB at 10 s and 7 GB at 1 m for one symbol. The raw event log costs about
the same as 1 s columns, less than 100 ms columns, and almost no CPU, while being the only
option that can rebuild any level and replay the book. Proposal for the owner's decision:
keep the raw log as the source of truth; keep 1 m and 1 h forever; keep 1 s rolling (about
30 days, ~4.4 GB); build 100 ms on demand from raw (or keep about 7 days, ~3.7 GB). Measure
the true exchange feed size before committing, since this stream is already coalesced.
(2026-09-29: the owner accepted raw as the source of truth; retention numbers here wait for
the pristine capture specified in "Owner decisions".)

Synthetic traffic is deterministic (4,000 initial levels, 50 single-level updates/sec,
static best-price neighborhood with rotating sizes). It is a regression fixture,
never a BTC capacity forecast. The 600-second run produced 30,000 messages
(one snapshot and 29,999 deltas), with no invalidations or ignored updates.

| Synthetic encoder, peak enabled | Encoded bytes in 600 s | Bytes/hour | Extrapolated MB/day | Encoder CPU seconds |
|---|---:|---:|---:|---:|
| Raw parsed L2 | 300,878 | 1,805,268 | 43.326432 | 0.00759 |
| 100 ms, near+deep | 1,202,572 | 7,215,432 | 173.170368 | 0.54286 |
| 1 s, near+deep | 359,226 | 2,155,356 | 51.728544 | 0.06058 |
| 10 s, near+deep | 226,278 | 1,357,668 | 32.584032 | 0.01751 |
| 1 m, near+deep | 87,990 | 527,940 | 12.670560 | 0.01026 |

The separate `--no-peak` control produced 1,270,262 / 438,320 / 226,052 / 74,357
bytes at 100 ms / 1 s / 10 s / 1 m respectively. It can be **larger**: the retained
schema-3 field codes `peakCode-twapCode`, so a zero peak can compress worse than a
peak near the mean. These values measure this compatibility-layout control, not
the marginal saving of a redesigned format that omits peak bytes. Raw bytes were
identical in both runs. The 100 ms/minute ratio with peaks was about 13.7, specific
to this quiet deterministic fixture and keyframe schedule.

Environment: macOS 26.6.2 arm64, `RelWithDebInfo`, zstd level 3. CPU times are one
run on shared hardware, not a throughput or latency guarantee. The peak-enabled
run overlapped the full test suite. Rates include initial/final overhead and do
not predict real traffic or the larger exact-sums representation.

Initial research validation: full `cmake --build --preset mac-clang -j 4` passed;
`ctest --test-dir build/mac-clang --output-on-failure` reported
`100% tests passed, 0 tests failed out of 38`. `StorageProbeTests` comprises ten
offline cases. Both synthetic CLI modes completed; missing-certificate failure
JSON and invalid-duration rejection were checked without opening a socket.
The existing dependency installation was reused with the local CMake cache setting
`VCPKG_MANIFEST_INSTALL=OFF`; the compiler cache and full tests required sandbox
access outside this worktree. No project dependency settings were changed.
Live BTC capture and serving/GPU performance remain unverified by this task.

## Live crash follow-up: write ownership and recoverable results

The owner's `f2a0b28` run crashed about 150 seconds after launch (02:42:54 to
02:45:24 EDT, 2026-09-29). The supplied `storage_probe-crash.ips` identifies the
`stream-client` thread in `onWrite -> doWrite -> buffer(queue.front()) -> string::size`,
with a near-null address, while the probe's main thread was waiting in Qt's event loop.

The write-start race exists even with one subscription and a single I/O thread:

1. `onHandshake` marks connected and emits the queued Qt `connected` signal.
2. The receiving thread posts `subscribe` onto the client strand.
3. The handshake callback posts its own deferred queue drain onto that strand.
4. The subscribe handler starts `async_write(queue.front())`.
5. The deferred drain sees a nonempty queue and starts a second write on the same
   head before the first completion has popped it.

A strand orders callbacks; it does not make overlapping asynchronous writes legal.
The first completion can release a payload still borrowed by the second operation;
duplicate completions can also consume an empty queue. This violates both the
one-write-at-a-time and buffer-lifetime contracts in the [Beast async_write
documentation](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/ref/boost__beast__websocket__stream/async_write.html).
The probe does not reconnect during capture, so its reported crash does not require
the reconnect queue-clear candidate. The encoder-only ASan tests pass; that is
evidence about those fixtures, not a proof about every possible stream. An isolated
copy of the original write methods, with a diagnostic assertion added, aborts in
the deterministic duplicate-drain test at the second write start with queue size
one. Without the diagnostic assertion the original methods hang during teardown
in this fixture; the exact delayed live SIGSEGV was not reproduced.

The client now drains pre-handshake messages synchronously before notifying
subscribers, has a strand-owned `m_writeInFlight` guard, binds write completions
explicitly to the strand, and checks ownership and queue emptiness before popping.
A failed write clears the in-flight flag, disables further writes and reports the
error rather than retrying an ambiguous partial message. Teardown closes the socket
and drains cancellation completions before clearing borrowed buffers. Reconnection
creates a fresh TLS/WebSocket transport: reusing the canceled transport failed the
new reconnect regression even after callbacks were drained.

The probe writes the same JSON path atomically with `QSaveFile` before connecting,
after the initial snapshot, every 60 seconds, and on normal/error exit. Status is
`starting`, `partial`, `complete`, or `failed`; `checkpoint_utc_ms` identifies the
saved checkpoint and `finalized` records whether the final flush succeeded. Each
minute also logs elapsed milliseconds, event count and all five encoder byte counts
through Qt's stderr handler. An unwritable output path fails before network startup.

Periodic reports include completed compressed blocks/columns only. They expose
`pending_raw_bytes` and leave forming TWAP columns open; they do not flush encoders
or change compression boundaries. Thus partial rates omit the current tail. A
process crash preserves the preceding checkpoint, not necessarily the last minute
of input. Final reports include the normal final partial-column/raw-block flush.
The synthetic mode checkpoints on simulated time, making kill/recovery tests fast.

New regressions use a local TLS/WebSocket peer for duplicate drains, cancellation
with an 8 MiB borrowed buffer, reconnect, failed writes and defensive completions.
The CLI tests kill a subprocess after its first minute checkpoint and read its
valid partial JSON, then verify that a completed ten-minute synthetic run preserves
the original byte totals despite checkpointing. The ASan configuration instruments
the probe, encoder tests, client and these regression tests using
`-fsanitize=address -fno-omit-frame-pointer` with `-O1 -g`; prebuilt Qt/OpenSSL/zstd
dependencies are not instrumented. Targeted ASan tests are run with
`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1`.

Follow-up validation: `cmake --build --preset mac-clang -j 6` passed; full
`ctest --test-dir build/mac-clang --output-on-failure` passed **42/42** suites
after rebasing onto `main` (including the two new candle suites).
The three targeted ASan suites (offline encoders, TLS client regressions and
probe checkpoint CLI tests) passed **3/3**, comprising 15 individual cases.
Only those targets and their dependencies were built under ASan. The owner
should rerun the 30-minute live capture; this follow-up does not claim a new
live BTC measurement or reproduction of the exact delayed fault.

## Prior art that changes a decision

- **kdb+/tick:** a tickerplant log supports rebuilding subscriber state. Borrow the
  separation between source log and derived serving state; it is a reason to keep
  raw if replay/recovery matters, not a reason to deploy kdb+ for this single-asset
  paint workload. [KX data recovery](https://code.kx.com/q/wp/data-recovery/).
- **ClickHouse:** `AggregatingMergeTree` stores aggregate states and merges them;
  `-State`/`-Merge` is the useful model for sums/durations, rather than merging
  finalized averages. It becomes an alternative when cross-asset analytics and
  operational SQL outweigh the simplicity of immutable local chunks. [Official
  documentation](https://clickhouse.com/docs/reference/engines/table-engines/mergetree-family/aggregatingmergetree).
- **Thanos:** downsampling keeps several aggregates (including sum/count/min/max),
  aims at query speed, and can increase total storage when all resolutions survive.
  Its compactor documents that retention can delete data before the next level is
  built, and that deleting fine levels removes historical zoom detail. Borrow those
  lifecycle constraints, not sample-count averaging for event-duration books.
  [Official compactor documentation](https://github.com/thanos-io/thanos/blob/main/docs/components/compact.md).
- **Bookmap:** its recorder documentation describes converting data to files for
  replay. That supports separating event replay from heatmap display, but does not
  establish a public TWAP codec or justify copying an undocumented binary layout.
  [Bookmap Recorder documentation](https://bookmap.com/wp-content/themes/bookmap/Bookmap_Connectivity.pdf).

**TapeSurf:** its public guide says cells aggregate book activity during the chosen
candle interval, supports price granularity and thresholds, and states FPS does not
control captured data. It does not disclose TWAP versus maxima, quantization,
retention, compression, or where aggregation happens. A server archive plus cached
multiresolution summaries and client-side intensity mapping is a plausible
architecture for these interactions, **an inference, not a finding about TapeSurf's
implementation**. Neither screenshots nor its marketing establish its backend or
prove its values are composable. Sentinel's independent heatmap timeframe remains
its own contract. [TapeSurf heatmap guide](https://tapesurf.com/learn/orderbook-heatmap).
