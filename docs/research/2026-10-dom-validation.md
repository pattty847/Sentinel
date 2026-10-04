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
every book event; the visible timer samples that complete replica. Hide and tab-visibility events stop the
timer; events keep entering ingestion state. A show catches up on the next timer tick. Explicit recenter is
an immediate user action. Normal updates do not reset the model or construct table items.

The header names the symbol, aggregation tick, base size units, quote price units, bucketed spread, and
execution count window. Numeric cells align right; rows are 22 px. Resting bid/ask use cyan/amber, executed
buy/sell use green/red, and side is also encoded by column labels/position. Columns retain readable widths
and use horizontal scrolling in narrow docks.

`DomFreshness` is intentionally replaceable by W2a: it uses `LiveOrderBook::getLastUpdate()` (the local receive
time supplied by RemoteGridDataSource), a 3 s age threshold, and existing connection/snapshot-stale signals.
Trade activity and painting cannot freshen a book. Reconnect waits for a book event; disconnect and stale
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

The 12 DOM checks cover unique same-bucket bid/ask and best ask, contiguous empty levels, numeric alignment,
base/quote/tick headers, empty/unchanged books, ring eviction, unknown side, counts independent of size,
manual anchor under rapid changes, execution interval bucketing and tick changes, low prices/nonfinite
inputs, receive-age freshness, hidden and tab-covered ingestion without model work, scroll/recenter,
coalescing under sustained events, reconnect/stale snapshot, and hidden symbol changes.

## Results

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

## Hand-off scope

No hubs, renderer, core or RemoteGridDataSource implementation changes. The only registration hooks outside
DOM implementation/tests are two source entries in `libs/gui/CMakeLists.txt` and the `DomTests` target in
`tests/render/CMakeLists.txt`. Canonical architecture documentation and shared agent invariants describe the
new contract. Changes remain uncommitted as instructed; the orchestrator must commit/rebase before landing.
The tested worktree base is `9aff385`.
