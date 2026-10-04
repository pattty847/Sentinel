# W1a DOM implementation and validation

Scope: directive P1; `lt-astra/dom`. Owner decisions in
[widget-pass-directive](2026-10-widget-pass-directive.md) take precedence over the audit.

## Behavior

`DomTradeWindow` owns a fixed 1,000-event ring, independent of the view. Every active-symbol execution enters
it, including trades arriving before the first book or while hidden. Unknown aggressors occupy the window,
are counted explicitly in its summary, and contribute to neither buy/sell counts nor delta. Execution size
does not affect execution counts. Invalid prices cannot be placed on a ladder row. Raw prices are retained so
a book aggregation-tick change can rebucket the same window.

`DomModel` publishes a retained `QAbstractTableModel` with 2,001 contiguous integer price buckets. There is no
synthetic midpoint row: both resting sides can occupy the same row. Empty buckets remain present. The
inspection window contains approximately 1,000 ticks on each side of the market, clamped at zero; it is not a
full-depth book browser. Scrolling pins that window and the scroll offset. Recenter / follow moves it back to
the market and restores automatic centering. Tick changes preserve the pinned top price to the new grid's
precision. Quantities come from the already-ingested GUI replica at its native aggregation tick; the model
does not reconstruct exchange-level prices or imply that a zero bucketed spread is a zero exchange spread.

The dock uses a 67 ms (~15 Hz) timer. Book/trade slots do no table or widget work. The datasource still applies
every book event; the visible timer samples that complete replica. Hide, tab-visibility and host-window minimization stop the
timer; events keep entering ingestion state. A show catches up on the next timer tick. Explicit recenter is
an immediate user action. Normal updates do not reset the model or construct table items.

The header names the symbol, aggregation tick, base size units, quote price units, bucketed spread, and
execution count window. Numeric cells align right; rows are 22 px. Resting bid/ask use cyan/amber, executed
buy/sell use green/red, and side is also encoded by column labels/position. Columns retain readable widths
and use horizontal scrolling in narrow docks.

`DomFreshness` is intentionally replaceable by W2a: it uses `LiveOrderBook::getLastUpdate()` (the local receive
time supplied by RemoteGridDataSource) only to display last-change age after change notifications. Feed
status comes from the retained connection state and snapshot-stale signals; a quiet connected book is not
classified as stale. Identical L2 messages, trade activity and painting cannot reset displayed change age. Reconnect waits for a book event; disconnect and stale
snapshot are explicit. Retained values remain visible with their status.

## Checks and reproduction

All build/test/benchmark commands below must be wrapped by
`~/Programming/Sentinel/scripts/dev/build-queue.sh --label lt-astra/dom -- ...`.
This sandbox uses the already-installed dependencies (`VCPKG_MANIFEST_INSTALL=OFF` in its local CMake cache)
and `CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp`; these are build-environment adjustments, not tracked config changes.

- Build: `cmake --build --preset mac-clang -j 4`.
- DOM suite: `ctest --test-dir build/mac-clang -R '^DomTests$' --output-on-failure -j 4`.
- Fail-without-fix checks: `python3 tests/render/check_dom_mutations.py`. Each mutation must compile and its
  behavior test must fail; source is restored in `finally`, then rebuilt and the DOM suite rerun.
- Benchmark: `build/mac-clang/tests/render/test_dom --gtest_also_run_disabled_tests --gtest_filter='DomDock.DISABLED_SustainedUpdateAndRasterPaintBenchmark'`.
- Full suite: `ctest --test-dir build/mac-clang --output-on-failure -j 4`.

The DOM checks cover unique same-bucket bid/ask and best ask, contiguous empty levels, numeric alignment,
base/quote/tick headers, empty/unchanged books, ring eviction, unknown side, counts independent of size,
manual anchor under rapid changes, execution interval bucketing and tick changes, low prices/nonfinite
inputs, connection state and separate last-change age, hidden and tab-covered ingestion without model work, scroll/recenter,
coalescing under sustained events, reconnect/stale snapshot, and hidden symbol changes.

## Initial implementation results (bce9c8e)

- Targeted DOM suite: 12/12 passed.
- Fail-without-fix checks: 8/8 mutations compiled and were rejected by behavior tests; restored suite passed.
  Mutations cover duplicate buckets, nearest-tick execution rounding, unknown-as-sell, broken ring eviction,
  forced follow, per-event painting, hidden work, and substituting paint time for book receive time.
- Full `mac-clang` build (`-j 4`): passed.
- Full ctest (`QT_QPA_PLATFORM=offscreen`, `-j 4`): **100% tests passed, 0 tests failed out of 90**;
  total real time **134.84 s**. DOM: **12/12 passed**, CTest time 2.60 s.
- GPU coverage caveat: the full log contains `GPU case skipped: metal backend: no MTLDevice (Metal unavailable)`;
  a passed CTest entry containing skipped GPU cases is not GPU verification.
- Sustained benchmark: **passed**. Production dark stylesheet, Qt `offscreen`, 650 x 600 dock,
  5.001 s, **49,990 book events + 49,990 trades**, **74 frames** (~14.8 Hz).
  Combined **model publication + full dock raster paint**: **mean 4.96086 ms**, **p50 4.71263 ms**,
  **p95 8.248 ms**, **max 8.50375 ms**. Each frame samples the fully-ingested replica and ring. The measured
  interval excludes event ingestion, includes all dock headers/table painting, and uses a QImage raster
  target; it is neither native window-compositor timing nor GPU latency. The final retained count is checked
  against the 1,000-event capacity. The timer cadence is separately exercised by the behavior suite;
  the benchmark invokes publication/paint at 67 ms deadlines for isolated frame-cost measurement.

Native macOS pixels, dock layouts and native paint latency cannot be verified in
this branch's sandbox (no window server). Offscreen QWidget raster timing is reported separately from native
visual/GPU verification. No owner GUI or running service is used.

## Initial implementation scope

No hubs, renderer, core or RemoteGridDataSource implementation changes. The only registration hooks outside
DOM implementation/tests are two source entries in `libs/gui/CMakeLists.txt` and the `DomTests` target in
`tests/render/CMakeLists.txt`. Canonical architecture documentation and shared agent invariants describe the
new contract. That implementation was committed by the orchestrator as `bce9c8e` after testing on base
`9aff385`. Review round 1 was committed by the orchestrator as `089f113`; review round 2 below remains uncommitted.

## Review round 1

- Shared `sentinel::roller::deriveNearTick` supplies the existing recorder near-grid calculation to GUI
  replicas: about 1 bp of the snapshot reference price, nearest 1-2-5 step, raised to a multiple of the quote
  increment when supplied. The stream currently exposes no quote-increment metadata; its replica call uses
  the helper's explicit unknown-increment path. BTC retains the configured 0.1 tick. Subscriptions clear the
  old replica and every accepted snapshot recomputes the tick. The recorder's BTC override and metadata
  checks remain unchanged.
- Tests deliver PEPE, FARTCOIN, DOGE, BTC, and a changed-price PEPE resubscription through the real
  `RemoteGridDataSource` snapshot signal, not a hand-selected model tick. Coarse/unavailable aggregation
  produces explicit copy. Trade rows use the replica's exact origin-relative truncation.
- Connection state is cached in `IGridDataSource` for late consumers, with a read-only snapshot-stale query;
  symbol changes re-read both. No change-age timeout claims feed silence.
- Follow stops only on ladder input gestures, not scrollbar value changes from layout or programmatic
  positioning. Cached native symbol strings remove trade-event conversion allocations; colors are static.
- Host/floating window state suspends the publish timer while minimized. Coalescing tests drive the actual
  67 ms timer's timeout signal explicitly and count publications, without elapsed-time upper bounds.
- Additional coverage: decimal-boundary execution alignment with nonzero book origin, crossed/locked books,
  quiet connected books, unchanged L2 messages, disconnect/reconnect, resize/programmatic scrolling,
  symbol-switch stale state, and minimize/restore with continuing trade ingestion.

Review-round targeted validation: **18/18 DOM tests**, **9/9 datasource tests**, **15/15 roller tests**;
**100% tests passed, 0 tests failed out of 3** CTest entries (4.70 s). Fail-without-fix validation:
**10/10 mutations rejected after successful compilation**, restored DOM suite **18/18 passed**.
The major regressions are exercised by `global_replica_tick` and `quiet_book_marked_stale` mutations.
Full `mac-clang` rebuild (`-j 4`): **passed**. Full ctest (`QT_QPA_PLATFORM=offscreen`, `-j 4`):
**100% tests passed, 0 tests failed out of 90**, total real time **156.98 s**; DOM CTest time **2.09 s**.
All builds, tests and mutation checks ran through the FIFO build queue. The full log still contains
GPU cases skipped because Metal has no available device; native visuals and GPU behavior remain unverified.
The benchmark above is the initial implementation's measurement and was not rerun in this review round.

Contained scope extensions: extracted the pure grid calculation in `libs/core/roller/Grid.*`; changed only
snapshot tick selection (and its includes) in `RemoteGridDataSource.cpp`; marked the existing snapshot-stale
getter as an interface override; added read-only connection-state retention and snapshot-state access in
`IGridDataSource`. Algo/trade-command paths and hub implementations are untouched.

An upstream limitation was found during the production-path audit:
`ServerDataModel::onLiveOrderBookInitialized` still quantizes its stream book at the global tick, and the server
serializes that already-bucketed book. A client cannot reconstruct prices already reduced to zero there;
review round 2 resolves the client presentation with the owner-directed server-tick floor and explicit
coarse state below. Per-product server precision remains a separate slice.


## Review round 2 (base 089f113)

- Non-BTC replicas now use `max(server tick, derived near tick)`; BTC retains its configured tick. The
  client never advertises precision finer than the already-quantised wire book. A helper exception retains
  the valid server tick and completes snapshot ingestion normally, without clearing the replica or emitting
  an empty book. Pending snapshot state is erased only after ingestion; an invalid server tick preserves it.
- The DOM suppresses the ladder and the entire bid/ask/spread summary when the effective tick exceeds 1%
  of the midpoint or a populated best level collapses to zero. Copy names tick, quote unit and symbol:
  `Server aggregation 0.1 USD is too coarse for DOGE-USD`. A received coarse book is not labelled waiting.
- Production signal-path tests use server-quantised wire prices: DOGE 0.1/0.1, FARTCOIN 0.7/0.8,
  PEPE 0/0; BTC 84000/84001 and ETH 2500/2500.5 continue to render. Resubscriptions recompute the tick.
  The tests also check replica side quantities, nonempty notifications and subsequent L2 updates, plus
  same-product transitions across the strict 1% threshold and helper-exception fallback.
- Quote increment remains unknown in the current stream. Without metadata the helper derives a 1e-9 tick
  around PEPE's 1e-5 raw price, below its 1e-8 exchange increment. This round intentionally does not change
  that helper policy; the server's 0.1 floor bounds the current replica. A subsequent server slice will
  publish per-product ticks/increments.
- Scope: replica snapshot tick selection/state completion, DOM model/copy, tests and documentation only.
  No server, protocol, algo/trade-command, services or deployment changes.

Targeted validation: **19/19 DOM tests**, **9/9 datasource tests**; CTest **100% tests passed, 0 tests failed
out of 2**, **2.76 s**. Fail-without-fix: **12/12 mutations compiled and were rejected by behavior tests**;
restored DOM suite **19/19 passed**. Finding 1 is guarded independently by `false_replica_precision` and
`coarse_positive_prices_rendered`; finding 2 by `discard_replica_on_derived_tick_failure`.
Full `mac-clang` rebuild: **passed**. Full ctest: **100% tests passed, 0 tests failed out of 90**,
**136.94 s**; DOM CTest time **2.09 s**. All validation used the FIFO build queue and `-j 4` for builds/ctest.
The full log contains Metal-unavailable GPU skips; passing CTest entries do not verify those skipped cases.
Native visuals/GPU behavior remain unverified in this sandbox; the initial-implementation raster benchmark
above has not been rerun this round.
