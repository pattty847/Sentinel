# Offline journal roller (slice A)

`sentinel-roll` reads RAWL2 v1/v2 and writes the existing HMC2 minute near/deep
and deep-hour series. It neither opens an exchange connection nor changes the
running server/capture. The existing live recorder API behavior, timer, queue-overflow policy,
writer defaults and server configuration are unchanged. Cutover remains slice D.

Build and invoke through the machine queue:

```sh
scripts/dev/build-queue.sh --label lt-astra/roller-a -- \
  cmake --build --preset mac-clang -j 4 --target sentinel_roll hmc2_diff
scripts/dev/build-queue.sh --label lt-astra/roller-a -- \
  build/mac-clang/apps/sentinel-roll/sentinel-roll \
  /Volumes/T7/sentinel-data/raw-l2 "$PWD/roll-out/example" \
  --products BTC-USD,PEPE-USD --from 2026-10-02 --to 2026-10-03
```

Dates are UTC, `--to` is exclusive, and ISO timestamps ending in `Z` are also
accepted. Endpoints must be minute-aligned. Both roots and the product list are
required; roots must be disjoint. `--dry-run` (alias `--report`) reads/parses the
input and reports the grid and input volume without creating output. Normal
stdout is JSON: processed records (including warmup/lifecycle/receipts), JSON
bytes, wall seconds, records/s, JSON MB/s, output bytes and daily commit cutoffs.
`SENTINEL_LOG_DIR` selects the normal Sentinel run-log directory.

The agent's measurements use only `roll-out/`; production backfill into the new
`/Volumes/T7/sentinel-data/hmc2` root is an orchestrator operation after review.
The CLI refuses the configured live `recording.dir` and `fallback_dir`, including
canonical aliases and overlapping roots. It reads `config/server_config.yaml`
and the server's optional `config/.server_config.yaml` from the working directory;
`--config` cannot bypass those exclusions. Outside a checkout, supply a config
containing `recording.dir`. Every selected product is checked before starting:
existing `.hmc2` files without `<output>/<product>/roller.json` are refused by
both the CLI and library, including dry runs. An interrupted first write before
any checkpoint therefore requires a fresh output root, not an unsafe adoption
of unowned files.

## Library and replay semantics

- `roller/JournalReader` inventories one product's headers and orders by run
  start, run id, segment. It reads one decompressed block at a time. Positions
  are `(product, run_id, block ordinal, record index)`, inclusive when reopening.
  If a saved position lies in a corrupt payload block, the reader returns its
  first readable successor with a gap instead. Callers that require the exact
  cursor must handle that recovery case explicitly.
  To poll an open file, reopen and skip the already applied position. Inventory
  refresh is explicit; this batch CLI does not wait forever for new records.
- Reader framing is shared with capture. The verifier's strict default remains
  unchanged. Roller replay and anchor searches skip complete payload CRC/zstd
  failures and damaged block headers/magic. A sealed file's CRC-checked closing
  index is read first; its entries must describe contiguous, bounded blocks with
  consecutive ordinals, and supply the boundary past a damaged header. With a
  missing/damaged index, the file is treated as unsealed: search in 64 KiB chunks,
  over at most one maximum legal encoded block span, for a complete BLK1 header
  with valid CRC, valid limits and exactly the successor ordinal. If none can be
  proved (including consecutive damaged headers), abandon the remaining captured
  suffix. Incomplete terminal blocks/indexes still defer as pending/torn tails.
  A skipped region sets the existing boundary gap; validity ends at the previous
  complete record until the next accepted exchange snapshot. No journal bytes
  are changed. Each distinct (product, run, segment, offset) is logged at error
  level and counted once per process by `sentinel_roller_journal_corrupt_blocks_total`.
  Record-layout corruption behind a valid payload CRC and file-header damage
  remain strict errors; an incomplete file header is retried after writer progress.
- Missing segments, torn superseded tails, run/connection changes, transport and
  applicable product-scoped validity markers invalidate observation. A discovered
  gap ends validity at the previous complete record's receive time. Only a new
  accepted snapshot restores it. V2 foreign events are filtered; kind-9 receipts
  do not provide book updates. Receipts with sequence gaps invalidate the book.
  A bad frame sequence invalidates that frame and re-anchors sequence tracking;
  a subsequent consecutive snapshot can restore the book without TransportUp.
- `JournalFeed` shares snapshot/update/zero-size filtering in
  `marketdata/dispatch/BookParser.hpp`, and trade parsing in `MessageDispatcher`.
  Missing or parser-rejected timestamps use journal receive time. Live callers
  retain their original host-clock fallback. No alternate integer L2 parser is used.
- The driver supplies receive milliseconds explicitly and emits one tick per
  journal record, including heartbeats, lifecycle markers and receipts. EOF is
  never a tick. No elapsed wall time or producer speed determines a column.
  At a day/range boundary, keep applying records until a drained checkpoint
  fence confirms the recorder committed through `end`, or until journal EOF.
  Receive time can lead the envelope-based integration clock, so a receive-time
  cutoff cannot prove that the final minute/hour committed. The output ceiling
  suppresses later buckets. Dry runs retain a receive-time scan bound for their
  input report and never claim committed progress.
- Offline queue admission blocks; a snapshot larger than the configured level
  budget is admitted alone rather than split or invalidated. RAWL2's record cap
  bounds its size. Default live admission still has its existing overflow policy.
  `requestStop()` wakes all blocked offline producers; callers join them before
  destroying the recorder. The destructor uses the same stop/broadcast path.
- Each day replays from its newest snapshot at/before UTC day start, or the first
  available later snapshot. This keeps the floating-point integration history and
  grid reference identical on restart. `commitFloorMs` suppresses already committed
  minute buckets, and a ceiling limits output to the requested range. Earlier
  book state is rebuilt; persisted minute records rebuild partial hours.

## Journal replay anchors

`AnchorStore` stores derived cache files at
`<output>/<product>/anchors/YYYY-MM-DD/HHMM.anchor`. Version 1 uses the
`SANCHR01` magic, little-endian format version and raw/compressed lengths, a
zstd-compressed CBOR payload, and CRC32 over the complete header and compressed
payload. Both compressed and uncompressed payloads are bounded to 128 MiB.
Identity includes product, UTC day, requested range, semantic config hash,
quarter-hour boundary and inclusive journal position. The original exchange
snapshot position, its mid and its product metadata preserve daily grid
provenance without needing to reopen the preceding day's snapshot file.
Feed, book and recorder sections each have their own version and opaque bytes.
The feed section carries JournalFeed continuity (run, connection, receive clock,
sequence and anchored flag), the native-price durable book and connection state,
and the recorder's original output floor. The recorder section includes its
quantized book as well as the complete integration state. The separate book
section is reserved in version 1; the two existing book representations are
validated by their consumers.

Writes validate first, durably create parent directories, then use a temporary
file, file sync, atomic rename and directory sync. Reads check the entire file,
CRC, compression framing, identities and section versions before returning a
candidate. Missing, corrupt, unsupported or incompatible files are cache misses
with a rejection reason. A supplied ceiling rejects later positions; cross-run
ordering requires journal inventory knowledge and conservatively rejects here.
Opaque section consumers must validate all sections before installing any state.
No RAWL2 records, exchange events or resync flags are generated by these APIs.

`BookRecorder::exportState()` drains the producer's accepted work and serializes
books, validity, integration clocks and offset, minute observations/flags/mids,
row size/integral/peak/timestamps/level counts and serials, the pending lateness
tail, partial hourly rollups, watermarks, publication state and recovery timers.
It includes diagnostic counters and rejects a recorder with dropped input or
disk errors. Row accumulators use classic-locale `max_digits10` long-double text;
CBOR preserves binary64 values and integer fields. The state has a CRC and version
and records floating-point precision/exponent limits; incompatible numeric
representations are rejected, not narrowed.

`importState()` requires a fresh recorder with identical policy/range/publication
configuration (output root and callbacks may differ). It validates and builds the
whole candidate before swapping it into the idle worker. It invokes no snapshot,
tick or publication callbacks. The existing durable HMC2 prefix must be present
in the destination; deterministic HMC2 reopen reconstructs its compression base.
Producer name interning is rebuilt lazily, queues are empty at the drain, and
per-message touched-row pointer lists are scratch. There is no per-record work
added to the primary recorder. Persisted minute entries in hourly accumulators
may carry decoded `coveredMs == observedMs`; fresh entries carry zero. Both are
valid and retained exactly, including when the grid snapshot precedes midnight.

Position-based `JournalReader` opens now pass the starting block to `RecordReader`.
A sealed segment's terminal CRC-protected index gives the block offset. RAWL2 has
no footer pointer, so locating that index reads at most 2,883,600 trailing bytes
(in addition to the existing closing-index probe on a potentially open file).
An unsealed segment has no index: seeking reads and validates its preceding
48-byte block headers and skips compressed payloads. Both paths decompress only
the selected block and the suffix; the selected block is the maximum extra record
work. `decodedRecords()` counts whole decompressed blocks, including records
before the cursor in the selected block. A seek trusts the skipped prefix and
does not verify its payload CRCs; default full scans still validate every payload.
This API is for a previously validated replay cursor. Subsequent block framing,
CRC and gap checks remain active, and the returned position is inclusive.

`roll()` exports after the first durable record at/after each intraday UTC
quarter-hour boundary. Midnight retains the state before the first new-day
record when warmup exists: its exact inclusive position can be in yesterday,
while a separate `resumeAt` points into today's file and is applied once. This
keeps minutes between midnight and a late first record reproducible without
yesterday's file. If the first usable snapshot itself arrives today, midnight
captures that first initialized state; there is no earlier output prefix.
No timer manufactures a journal record or advances integration. On restart it
validates the newest compatible candidate at/before the checkpoint, imports the
state, and applies only records after its inclusive position. Normal anchors
consume/discard the saved cursor; midnight's `resumeAt` is returned to the reader
for one application. Canonical filenames and the checkpoint's receive high-water
prefilter future sidecars before decompression (old checkpoints get a bounded
cursor read). Reports count actual decoded journal records, including selected
block prefixes, fallback snapshot scans and rejected cursor checks. The live source additionally checks its handshake ceiling and
can restore exactly at that ceiling without asking disk for another record.
The model tap receives its raw book directly, with no snapshot callback during
restore; its normal socket-tip seed remains the only model snapshot. A restored
writer at the durable tip also restores its running/receive metrics and reaffirms
proven committed progress, without requiring a future minute commit.

Validation covers the sidecar, policy/range, grid provenance, feed, raw book,
recorder and actual cursor before any external consumer is attached. Cross-run
checkpoint comparisons conservatively miss. Missing or invalid candidates use
the existing exchange-snapshot replay. With no committed output prefix, only a
midnight anchor with the original range floor qualifies. A fallback warming up
below an existing checkpoint does not overwrite earlier anchors with state
belonging to a different output prefix. Original midnight anchors are preserved;
if one is lost, rebuild it from the original journal while the preceding snapshot
is still available. Without that snapshot or a valid midnight anchor, a lost
book cannot be invented.

Before installation, the reader validates any overlap through the existing
checkpoint. It retains those decoded records for one later application (bounded
to a conservative 256 MiB estimate including JSON metadata), so validation does
not double the decoded-record count. Damage or an unavailable/oversize committed
overlap selects snapshot fallback: otherwise changed RAWL2 could disagree with
already committed HMC2 and repeatedly fail duplicate verification. No consumer
has been attached when this fallback is chosen.

The original recorder output floor travels with its state. Replay overlap uses
HMC2's existing exact-duplicate verification and reconstructs its compression
base from committed files. Already committed finals are filtered at publication;
restored statistics are excluded from the current roll report's column count.
The recorder's hour accumulator and lateness tail remain intact. Neither journal
format, HMC2 format, grid rules nor roller configHash changes. All roll recorders
use Finals publication policy, including batch/rebuild with no publisher (then
All and Finals have identical behavior). Rebuilt anchors therefore import into
the live history recorder without weakening state-policy validation. At the end
of a requested range, checkpoint fences also run once per journal second until
lateness closes; waiting for the next minute unnecessarily extends the scan.

`sentinel-roll rebuild-anchors JOURNAL_ROOT OUTPUT_ROOT --products BTC-USD --from
2026-10-05 --to 2026-10-06` rebuilds from RAWL2 using a disposable HMC2 root,
without consulting old anchors/checkpoints. Only sidecars are installed in the
requested output. Existing root exclusions apply; the destination writer lease
is held for rebuilding too. Use `--no-anchors` with normal replay for the
exchange-snapshot baseline. Both commands retain the existing config options.

Midnight sidecars are retained forever. At completed/cancelled day passes the
store removes only recognized intraday sidecars more than two days behind the
newest durable receive time actually replayed (never the future end of a cancelled
range). This journal-time horizon also lets historical rebuilds
retain their own intraday set for verification. It never traverses journal or
HMC2 stores; symlink directories/files are excluded from pruning.

Validation commands (all through `scripts/dev/build-queue.sh`, build `-j 2`):

- `ctest --test-dir build/mac-clang --output-on-failure -R '^JournalAnchorTests$'`
  runs synthetic boundary, fallback, retention, CLI rebuild, feed state, recorder
  state and bounded reader tests.
- `SENTINEL_ANCHOR_REAL_ROOT=/Volumes/T7/sentinel-data/raw-l2` enables the BTC and
  PEPE captured-hour recorder export/import cases in that suite.
- `SENTINEL_ANCHOR_DAY_ROOT=/Volumes/T7/sentinel-data/raw-l2
  build/mac-clang/tests/roller/test_journal_anchors
  --gtest_filter=AnchorReplay.RealDayEveryAnchorMidnightAndSevenProductBenchmark`
  runs the explicit long captured-day acceptance: 96 separate resumptions for
  BTC and PEPE, a source view without yesterday, exact HMC2 file bytes plus strict
  minute/hour comparisons, decoded counts and seven-product restart timings.
- `python3 tests/roller/anchor_mutations.py` and
  `python3 tests/roller/shadow_mutations.py --db1` run fail-without/restored checks.

Captured inputs are opened read-only; all output is temporary. The captured-hour
Phase A export measured BTC recorder state 11,249,023 bytes / compressed sidecar
1,434,237 bytes and PEPE 215,623 / 61,353 bytes (feed/raw-book sections empty in
that primitive test). The full-day harness prints populated sidecar size ranges
and restart/decoded-record evidence per product.


Phase B validation on mac-clang, 2026-10-07, used the captured 2026-10-05 day.
Each BTC and PEPE quarter was a separate roll lifetime with the preceding day
absent from its read-only source view. All 96 imports per product matched full
replay byte for byte and had strict `hmc2_diff = 0` for near/deep minute/hour
series. BTC's maximum replay span was 901,908 ms / 21,348 decoded records;
PEPE's was 901,697 ms / 6,805 decoded records. Counts include the selected
block's decoded prefix. The captured-day test passed in 641.772 seconds.

The following are single paired restart measurements at the final committed
minute, with the same already-durable output prefix. Before uses exchange
snapshot replay; after uses the newest qualifying sidecar. Sizes cover all
96 populated sidecars for each product (bytes, including feed and raw book).

| Product | Before (s) | After (s) | Decoded before | Decoded after | Sidecar bytes, min-max |
|---|---:|---:|---:|---:|---:|
| BTC-USD | 48.556 | 2.597 | 2,428,286 | 18,768 | 1,577,270-2,685,682 |
| PEPE-USD | 6.145 | 0.252 | 702,247 | 4,195 | 80,518-91,742 |
| ETH-USD | 40.146 | 0.958 | 2,472,329 | 16,224 | 652,799-841,334 |
| SOL-USD | 19.796 | 0.837 | 1,796,455 | 13,461 | 482,451-645,442 |
| DOGE-USD | 15.666 | 0.632 | 1,520,078 | 7,076 | 257,927-396,640 |
| AVAX-USD | 11.870 | 0.609 | 1,079,521 | 8,835 | 217,423-350,036 |
| FARTCOIN-USD | 8.879 | 0.451 | 799,803 | 4,259 | 65,380-146,033 |

The final full queued mac-clang build passed at `-j 2`. The queued regression
run passed 25/25 targeted suites. After the final
changes and mutation restoration, all six capture/roller suites passed again
(216.02 seconds), including the captured-hour corrupt-block test. The separate
4/4 integration pass included real BTC/PEPE recorder export/import.
`anchor_mutations.py` killed 23/23 mutants; `shadow_mutations.py --db1` killed
39/39, each in a complete run with every restored check passing. These cover
state fidelity, strict parity, production/selection of anchors, midnight and
late-first-record independence, decoded and lateness scan bounds, fallback
and checkpoint/handshake ceilings, rebuild, retention, and durable tap restore.
Local evidence is in `build/mac-clang/anchors-b-{final-tests,final-integration,
restored-tests,mutations,db1-mutations,real-final}.txt`.

## Daily grids and overrides

Reference price is the two-sided mid of that day's replay-anchor snapshot. The
nearest arithmetic-distance `{1,2,5} * 10^k` step to one basis point is selected
(ties round upward), then rounded up to a multiple of the quote increment. It
never falls below that increment. This remains the default near tick. The default
deep tick is two strictly higher 1-2-5 ladder steps above that near tick, then
rounded up to a quote-increment multiple. If near lies between rungs, count the
next higher rung as step one (e.g. native increment 0.03 gives near 0.03, then
0.05 -> 0.1, rounded to deep 0.12). Explicit near/deep overrides remain independent
and take precedence over these defaults; BTC retains near $1 / deep $5. The grid
stays fixed for the entire recording day.
Price scale is the power of ten needed to represent the quote-increment decimal;
size floor is `base_increment`. Unsupported precision/overflow is rejected.

Representative table (prices are test inputs, increments are archive metadata):

| Product | Reference price | Quote increment | Price scale | Near tick | Deep tick |
|---|---:|---:|---:|---:|---:|
| BTC-USD | 100000 | 0.01 | 100 | 1 | 5 |
| ETH-USD | 4000 | 0.01 | 100 | 0.5 | 2 |
| SOL-USD | 200 | 0.01 | 100 | 0.02 | 0.1 |
| DOGE-USD | 0.2 | 0.00001 | 100000 | 0.00002 | 0.0001 |
| PEPE-USD | 0.00001 | 0.00000001 | 100000000 | 0.00000001 | 0.00000005 |
| FARTCOIN-USD | 1 | 0.00001 | 100000 | 0.0001 | 0.0005 |
| AVAX-USD | 30 | 0.001 | 1000 | 0.002 | 0.01 |

`--config <yaml>` reads the roller's `recording.products.<id>` overrides
(and live-root exclusions described above):
`near_tick`, `deep_tick`, `price_scale`, `size_floor`. Tick overrides are also
clamped/rounded to native increments. This does not change ServerDataModel's
live/global config interpretation. To change an already checkpointed policy or
extend a day's start earlier, use a new output root; incompatible resume fails.

```yaml
recording:
  products:
    BTC-USD:
      near_tick: 1
      deep_tick: 5
```

## Checkpoints, recovery and failures

`<output>/<product>/roller.json` records version, position, semantic config hash
and exclusive committed-through milliseconds. A `days` map retains the daily
states. A fence drains accepted work and verifies disk/queue status; only then
is the checkpoint written using a temporary file, file fsync, rename and directory
fsync. Fences run at receive-minute changes and at the end of the batch, after
HMC2's durable writes. Thus a crash may leave durable columns ahead of the
checkpoint; it cannot claim non-durable columns. The named journal position is
validated on resume. Open/lateness-tail state is re-derived, never serialized as
observed history.

The HMC2 writer's opt-in deterministic-resume mode validates the whole existing
file and restores its final decoded delta base. An already present bucket must
be byte-identical when encoded absolutely, or replay fails; an identical one is
suppressed. This also handles a crash after one layer was appended but before
the other. Normal live reopen behavior still clears the delta base. Store locking,
CRC rejection, torn-terminal-frame recovery and fsync use the existing primitives.
The separate mode is necessary for physical byte identity after resume; generic
last-record-wins appends alone only provide decoded idempotence.

## Decoded comparison and controlled tick-schedule tolerance

```sh
scripts/dev/build-queue.sh --label lt-astra/roller-a -- \
  build/mac-clang/apps/sentinel-roll/hmc2_diff ROOT_A ROOT_B BTC-USD near \
  2026-10-01T16:00:00Z 2026-10-01T17:00:00Z
```

Optional final argument `3600000` compares deep hours. A qualifying minute exists
on both sides, has `observedMs == 60000`, and has no `kResynced`. `kLateEvents` is
masked. Header grid/size scale, metadata, bounds, sparse TWAP/peak codes and
coverage are compared exactly. Hours require all 60 constituents to qualify.
JSON lists every minute, exclusion reasons, and diagnostics for every qualifying
minute. `difference.twapCodeDeltaHistogram` counts signed **B minus A** code
deltas for common `(row, side)` entries, including zero. `onlyA`/`onlyB` count
unmatched entries separately. `difference.totalTwap.bid/ask` contains decoded
base-unit totals `a`, `b`, `delta = b - a`, and `relativeDelta = delta / a`
(null when `a == 0`); totals include unmatched entries. Exit 2
means qualifying mismatches; exit 1 means a fatal invocation error. The existing
HMC2 range reader logs unreadable files and excludes their missing buckets; a
zero-qualifying result is not a parity pass.

**The plan's claim that switching ticks leaves minute content unchanged is false.**
On the checked-in three-minute captured BTC fixture, the legacy API with 250 ms
local-time ticks and journal replay with record-time ticks produce two qualifying
minutes with identical mids, bounds, row sets and peaks, but 24 and 4 differing
TWAP codes (maximum deltas 11 and 23). The first minute is partial. `advance()`
makes the integration clock monotone; an idle tick can advance it beyond a later
arriving envelope, changing that event's effective integration time. Masking
`kLateEvents` does not undo that arithmetic.

The real independent-feed hour 2026-10-01 16:00-17:00 UTC has **58 qualifying,
0 matching, 58 mismatching, 2 nonqualifying minutes per layer**. Those differences
also include mids and peaks. Independent subscriptions and their batching are an
additional possible cause, not isolated by that comparison. No legacy-parity pass
is claimed. The orchestrator's review round accepted this finding and replaced
an exact cross-connection gate with a controlled same-input regression: mids,
bounds, peaks and row sets match exactly; at most 1% of entries differ in TWAP
code, maximum absolute delta is 32 codes, and decoded total TWAP per side differs
by at most 0.01%, checked per qualifying minute on both layers. The checked-in
fixture passes those bands. The same-journal byte-identity gate remains strict.
The corresponding deep hour has zero qualifying hours and one nonqualifying
hour, because two constituent minutes fail qualification.

## Measurements (owner's Mac, 2026-10-03)

Serial queued runs, raw input read-only, output under `roll-out/`:

| Product/day UTC | Processed records | JSON bytes | Wall seconds | Records/s | HMC2 bytes |
|---|---:|---:|---:|---:|---:|
| BTC 2026-10-01 | 1,816,917 | 3,193,014,327 | 53.745 | 33,806 | 18,287,442 |
| PEPE 2026-10-02 | 458,571 | 185,745,175 | 14.646 | 31,311 | 1,157,235 |

These include snapshot warmup and durable file/checkpoint writes; queue wait is
excluded. The day cutoffs reached the requested exclusive end. These measurements
predate the owner's deep-grid decision: PEPE used scale 100000000, both ticks
1e-8 and size floor 1; the current default deep tick is 5e-8. No service was
restarted or deployed.
One measured BTC day replays in under a minute, so hourly serialized book-state
checkpoints are not added in this slice; this is a measured threshold, not a
future throughput guarantee.

Header-only inventory at measurement time: 17,152,734 records across 409 files,
seven products, about 21.78 GB of uncompressed blocks. Extrapolating the slower
31,311 records/s gives **548 seconds (9.1 minutes)** for the then-current archive;
budget **10-15 minutes** for daily warmup, product differences and I/O variance.
Only BTC and PEPE were benchmarked. The full backfill was not run.

A second full BTC day on the final code took 52.348 seconds (34,708 records/s).
All three HMC2 files and the checkpoint were byte-identical to the first run;
the comparison is saved in `roll-out/real-day-determinism.json`.

## Regression evidence

`RollerTests` covers grids/clamping, v1/v2 positions, unsealed/torn/interior tails,
product/lifecycle/clock behavior, run-to-run bytes, crash resume across an hour,
idempotent rerun, dry-run/EOF, bounded blocking admission, commit bounds and decoded
comparison. `RollerLiveFixtureTests` pins the legacy-path bytes against pre-change
`BookRecorder.cpp` and `Hmc2Store.cpp` compiled separately during development.
The fixture and hash provenance are in `tests/roller/fixtures/README.md`.

Run `tests/roller/check_mutations.py` through the build queue to disable each
behavior, rebuild, require the relevant test to fail, restore/touch/rebuild, and
require it to pass. The final restored suites must pass (FM-132). The script does
not write secondary diagnostic logs. Existing recorder/capture tests remain
required for changes to the shared defaults.

The batch driver processes products/days serially because the current HMC2 lock
covers the root. Concurrent live hosting of different per-product configs in
slice C still needs a shared writer or an explicit lock-ownership design; this
slice does not weaken the existing lock.

Final validation (2026-10-03): full queued mac-clang build passed; CTest reported
`100% tests passed, 0 tests failed out of 83` in 353.73 seconds. Metal-dependent
cases explicitly skipped because no MTLDevice is available in the sandbox; no
GPU/visual claim is made. All ten new test cases passed. Fourteen targeted
mutations failed as expected and passed after restore/touch/rebuild (the first
twelve as a batch, the daily dry-run reference and superseding-run mutations
separately). The superseded-tail test was strengthened after the full run;
both roller suites passed again after that test-only change.


## Slice A review fixes (2026-10-03)

The follow-up to `11dee5c` adds day/range-boundary silence coverage, live-root
refusal, sequence recovery and blocked-producer shutdown regressions. The real
BTC prefix now also exercises crash resume with thousands of entries and a
durable minute ahead of its checkpoint; resumed HMC2 files match the clean run
byte for byte. All roller tests use auto-cleaned `QTemporaryDir` directories
beneath the CMake build directory, with no source-tree output dependency.
The 16 roller cases pass, including the unchanged legacy fixture hash. All nine
review-specific mutations failed with the behavior disabled and passed after
source restoration, touch and rebuild: boundary finalization, unowned-root
refusal, configured-root refusal, sequence recovery, stop broadcast, large delta
resume, histogram sign, total-TWAP delta sign and controlled-fixture bands.
Run `tests/roller/check_mutations.py --review` through the build queue to repeat
those checks. The subsequent owner decision (round 2, item 4) is implemented in
the daily grid derivation and table above; the near 1 bp rule is unchanged.


Review validation: full queued mac-clang build passed; CTest reported
`100% tests passed, 0 tests failed out of 83` in 340.82 seconds. The 16 roller
cases passed; all nine review mutations failed as intended and passed after
restoration/touch/rebuild. Metal-dependent cases skipped in the sandbox.
No HMC2/RAWL2/checkpoint files remained in the build-directory test scratch area.

The new diagnostics on the retained identical-input fixture outputs report:

| Layer/minute UTC | Entries | Differing TWAP codes | Differing fraction | Max code delta | Max absolute side-total delta |
|---|---:|---:|---:|---:|---:|
| near 04:54 | 4587 | 24 | 0.523218% | 11 | 0.000140795% |
| near 04:55 | 4781 | 4 | 0.083665% | 23 | 0.000000139% |
| deep 04:54 | 18369 | 3 | 0.016332% | 3 | 0.000001514% |
| deep 04:55 | 18398 | 0 | 0% | 0 | 0% |

Mids, bounds, peaks and row sets are exact. Full histograms/totals are in the
local `roll-out/review-controlled-stats.json`; tests regenerate both outputs
from the checked-in fixture and enforce the requested bands independently.


## Envelope-clock boundary regression (2026-10-03)

The initial boundary fix still stopped too early when receive time led the
recorder's integration clock. A writing roller now stops only when a drained
fence has persisted a checkpoint through the requested end, or at journal EOF.
The new captured 16:00 boundary fixture retains original records/timestamps;
restoring the receive-time cutoff fails its last-minute/hour assertion. The
fixture's provenance and compression-only reblocking are documented in
`tests/roller/fixtures/README.md`.

Re-rolling the complete BTC archive for 2026-10-01 15:00-16:00 UTC into
`roll-out/review-boundary-watermark` produced:

| Series | Records | Last bucket UTC | observedMs | Entries |
|---|---:|---|---:|---:|
| near 1m | 60 | 15:59 | 60000 | 4754 |
| deep 1m | 60 | 15:59 | 60000 | 18388 |
| deep 1h | 1 | 15:00 | 3526561 | 18607 |

The hour is partial/resynced according to the captured intervals; it is persisted
without inventing observation. `committedThroughMs` is 1790870400000 (16:00 UTC).
The rerun wrote zero columns and preserved all three HMC2 files and checkpoint
bytes. Evidence: `roll-out/review-boundary-watermark-evidence.json`. The initial
roll took 33.836 seconds including day-anchor warmup; no production root was
written and no service was changed.


Boundary-fix validation: the new captured-hour assertion failed with the old
receive-time cutoff restored and passed after source restoration, touch and
rebuild. Both roller suites passed (17 cases). The full queued mac-clang build
passed. Full CTest passed 84/85 suites in 352.78 seconds; the untouched
`RecordingServerStop.ActualServerStartStopStartRestoresLiveDelivery` test failed
its listener check at line 324 with repeated Bad file descriptor accept errors.
The queued `RecordingServerStopTests` rerun passed unchanged (1/1 suite, 8.99 s).
This intermittent server lifecycle failure is recorded separately as FM-154;
no server lifecycle source was changed for the boundary fix. Metal-dependent
cases explicitly skipped in the sandbox. Both real-roll logs had zero W/E/F
lines, and the final diff passes whitespace checks.

Corruption acceptance in `RollerCorruptBlockTests` includes an env-gated real-hour
test: set `SENTINEL_CORRUPTION_HOUR` to a read-only BTC hour containing recovery
snapshots (the acceptance run uses `2026/10/05/17.rawl2`). The test copies it to
scratch before changing bytes; without that variable the real-hour cases skip.
`tests/roller/corrupt_block_mutations.py` is a Mac dev tool: run it inside
`scripts/dev/build-queue.sh`, with the real-hour variable set for those cases. It
builds the `mac-clang` preset at `-j 2`, restores each source mutation and rebuilds
before checking the restored baseline.
