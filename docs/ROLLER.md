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
  To poll an open file, reopen and skip the already applied position. Inventory
  refresh is explicit; this batch CLI does not wait forever for new records.
- Reader framing is shared with capture. The verifier's existing strict default
  remains unchanged. A roller reader defers an unframed/incomplete terminal tail
  while the file is unsealed and unsuperseded, even if later block-looking bytes
  are visible. A valid closing index, subsequent segment or newer run makes that
  tail terminal: later valid framing is then interior corruption. Complete CRC,
  zstd and framing errors still fail. A concurrently incomplete file header is
  an invocation error to retry after the writer finishes the header.
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
