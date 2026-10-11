# S8 zoom extraction evidence

Base: `401fd26`. Branch: `lt-astra/s8`. Neutral; no golden changed.

`ZoomController` owns click/continuous zoom, gesture position, the lazy settle timer,
wheel carry and axis wrappers. `WheelInterpreter` has no Qt dependency. UGR retains
`event()` and `wheelEvent()`, with token-identical bodies except policy/type
qualifications. `ChartViewController` owns and exposes the policy without new view
arithmetic. `PolicyTimer` factors the identical single-shot adapter shared with
`PriceCarryPolicy`; its carry-wait explanation is restored from `2ec42ea`.

## Statement map

Original ranges below are in `libs/gui/UnifiedGridRenderer.Zoom.cpp` at `401fd26`.
Destinations are in `libs/gui/chart/core/ZoomController.cpp`, except where named.
Unchanged expressions keep their operand order. Camera/state/port qualifications,
request signals, helper calls and value returns are the mechanical differences.

| Original lines | Destination function:line |
|---|---|
| 21–27 | `continuousFactor:14` (same constants/expression) |
| 71–94 | `setZoomRung:111` |
| 96–128 | `zoomStoredView:136`, with D7/D9/D10 helpers below |
| 130–143, 182–198 | `zoomClicks:166` |
| 144–170, 181 | `clickTarget:82` |
| 171–180 | `clickPriceFallback:68`, directional `clampSpan:57` |
| 200–202 | `zoomContinuous:238` |
| 204–208 | `canZoomContinuous:199` and `zoomContinuousFor:242` |
| 209–235 | `zoomContinuousFor:242` |
| 236–265 | `applyContinuousView:206`, with D9/D10 helpers |
| 266–272 | `zoomContinuousFor:242`, timer factory, `PolicyTimer.hpp:11` |
| 275–316 | `endZoomGesture:280`, with D7/D8 helpers |
| 319–327 | `WheelInterpreter.hpp`, `classify:7` |
| 318, 328–344 | `wheelZoom:318` |
| 347–350 | `zoomTimeWheel:340` |
| 352–356 | `zoomPriceWheel:345` |
| 358–360 | `zoomTimeDrag:351` |
| 362–365 | `zoomPriceDrag:355` |
| 367 | `zoomTimeClicks:360` |
| 369–372 | `zoomPriceClicks:362` |

## Dedupe expression evidence

- **D7:** old 97–98, 145–146, 211–212, 288–289 -> `zoomFrac:36`.
  Every X site computes `clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0)`;
  every Y site computes `clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0)`.
  End-gesture supplies the same saved gesture coordinates. X is evaluated before Y.
- **D8:** old 151–155 and 290–294 -> `columnLimits:42`.
  Both retain `max(1, int(ceil(W * tf / maxTime - 1e-9)))` and
  `min(maxColumnPixels(tf), minTime > 0 ? max(1, int(floor(W * tf / minTime + 1e-9))) : kMaxCellPx)`,
  including the same positive-limit guards and fallback values.
- **D9:** old 103–104, 112–113, 247–248, 256–257 -> ordinary `clampSpan:51`.
  Each retains `min(span, maximum)` followed by `max(span, min(current, minimum))`;
  current is exactly `double(end - start)` for time and `hi - lo` for price.
  The division that produces span remains at each call site. Old 177–178 ->
  directional overload `clampSpan:57`: zoom-out keeps
  `min(span, max(current, maximum))`; zoom-in keeps
  `max(span, min(current, minimum))`. The click-direction guards are unchanged.
- **D10:** old 125–126, 193–194, 264–265 -> `postZoom:63`.
  The three callers pass respectively `price`, `zoomPrice`, `price`. Each emits
  `priceInteracted` first, then conditionally calls `enableAutoScroll(false)` once.

A source-expression check passed all four dedupes and both shell event bodies.
The helpers do not introduce floating-point reassociation. Span-limit reads are
plain value getters; factoring them into arguments has no intervening mutation.

## Emit/connect comparison

- Three old `priceInteracted` sites become one helper with the same three call
  paths and guards. Runtime emission count and ordering are unchanged.
- Four old direct `update()` sites in the moved methods become four internal
  `updateRequested` sites, with one direct shell connection calling `update()`.
  The additional internal signal is the only new connection route.
- Existing glide connections remain in their original relative order:
  raster, viewport, polish, update. `ZoomGlide` itself is unchanged.
- Stored fallback / continuous commit order stays viewport, optional price
  interaction, optional follow-disable, then update. Click order stays viewport,
  optional price interaction, optional follow-disable, then start-axis-glides.
  `LivePlacement::enableAutoScroll` and all its shell connections are unchanged.
- The lazy timer still has one timeout connection per timer instance, calls
  `endZoomGesture`, restarts at 160/1000 ms as before, and stops before settling.
  Price carry still has its own timer instance and timeout connection. Sharing
  `PolicyTimer` merges source text, not runtime timers.
- Teardown stops the zoom timer and disconnects its shell request before port
  detach. UGR's reused polish timer stays in the shell.

## Baseline tests and mutations

Before extraction, a temporary adapter compiled the actual old zoom method bodies
against existing QtCore policies and fixed surface/tick ports. Only shell getters,
update delivery and the timer transport were adapted. The original classification
expression was exposed separately; wheel tests also exercised its original branch.
No application, event loop, QtGui or QtQuick was needed. These are QtCore-only
policy tests; `WheelInterpreter.hpp` additionally compiled by itself with plain
`clang++ -std=c++20 -fsyntax-only` and no Qt include path.

Baseline: **17 tests passed**. All 17 assigned mutations failed as expected; the
adapter was restored and all 17 passed again before the move. The final tests
also passed against the preserved old adapter (including the strengthened exact
fallback bounds assertion). The adapter is not part of the delivered source.

The classifier truth table covers 7 angles × 5 phases × 2 pixel-delta states × 2
precision states (140 fixed combinations). Gesture tests inject a clock and timer,
check deadline restart, the instant before expiry, and settling at expiry. Other
cases pin rungs, selected-axis bounds and anchors, stored fallback, limits, click
accumulation, auto-price ownership, gesture joins/end, carry reversal/route/axis
reset, the four-click cap, and all six axis wrappers.

Mutation output, one line per test (each mutation restored):

```text
MUTATION FAIL (expected): WheelInterpreterTest.EveryClassificationCombination | static Kind classify: return Kind::Ignore;
MUTATION FAIL (expected): ZoomTest.SetRungChangesOnlySelectedAxes | bool LegacyZoom::setZoomRung: return false;
MUTATION FAIL (expected): ZoomTest.StoredViewClampsAndPreservesSignalOrder | void LegacyZoom::zoomStoredView: return;
MUTATION FAIL (expected): ZoomTest.StoredViewZeroSurfaceUsesCenter | void LegacyZoom::zoomStoredView: return;
MUTATION FAIL (expected): ZoomTest.ClicksRejectZeroDraggingAndUnselectedAxes | void LegacyZoom::zoomClicks: update(); return;
MUTATION FAIL (expected): ZoomTest.ClicksUseStoredFallbackWithoutCamera | void LegacyZoom::zoomStoredView: return;
MUTATION FAIL (expected): ZoomTest.ClicksAccumulateFromTargetAndKeepAutoPrice | void LegacyZoom::zoomClicks: return;
MUTATION FAIL (expected): ZoomTest.ClicksPriceRungAndDirectionalFallback | void LegacyZoom::zoomClicks: return;
MUTATION FAIL (expected): ZoomTest.ClicksAtLimitsLeaveViewAndSignalsAlone | void LegacyZoom::zoomClicks: update(); return;
MUTATION FAIL (expected): ZoomTest.ContinuousTimerRestartsAndSettles | void LegacyZoom::zoomContinuousFor: return;
MUTATION FAIL (expected): ZoomTest.ContinuousAxesJoinAndClamp | void LegacyZoom::zoomContinuousFor: return;
MUTATION FAIL (expected): ZoomTest.ContinuousRejectsInvalidAndFallsBackBeforeGesture | void LegacyZoom::zoomContinuousFor: update(); return;
MUTATION FAIL (expected): ZoomTest.EndGestureSettlesAndClickEndsGestureFirst | void LegacyZoom::endZoomGesture: return;
MUTATION FAIL (expected): ZoomTest.EndGestureNoOpAndInvalidCamera | void LegacyZoom::endZoomGesture: update(); return;
MUTATION FAIL (expected): ZoomTest.WheelCarriesPartialOnlyAndDropsRouteAxesReversal | void LegacyZoom::wheelZoom: return;
MUTATION FAIL (expected): ZoomTest.WheelPixelNotchesPrecisePhasesAndEnd | void LegacyZoom::wheelZoom: return;
MUTATION FAIL (expected): ZoomTest.AxisWrappersPreserveOwnershipAndSafetyTimeout | void LegacyZoom::zoomPriceWheel: return;
```

## Verification

All builds/tests used `scripts/dev/build-queue.sh --label lt-astra/s8 -- ...`;
builds used `-j 6`, worktree-local `CCACHE_DIR`, and the cache has
`VCPKG_MANIFEST_INSTALL=OFF`. No production app or service was launched or deployed; GUI tests ran offscreen.

- Full `cmake --build --preset mac-clang -j 6`: exit 0.
- G plus ChartCamera, ChartViewController, PriceFitter, LivePlacement,
  TimeframeCarry, TimeframeSwitch and ZoomController: **20/22 CTest targets
  passed; two require native Metal** (34.33 s).
- `ChartViewTrace: PASS`: byte-identical checked-in trace; no re-bless.
- S1–S7 policy targets: **50 test cases passed** (11 camera, 2 controller,
  8 fitter, 11 placement, 14 carry, 4 timeframe).
- `ZoomControllerTests`: **17 test cases passed after extraction**.
- Canonical chart-health: **PASS**. FM-263 working-tree variant: **PASS**,
  including untracked files in both changed-file discovery and clone scanning.
- chart-health self-test: **PASS** (no-op and verbatim move pass; added branches
  and outside UGR include fail; unrelated C++ passes). Its wheelEvent target remains.
- Maximum extracted zoom CCN: **14**; `ChartViewController.cpp`: **4**.
- UGR `m_` declaration-line count: **69 -> 65** (canonical counter).
  `UnifiedGridRenderer.Zoom.cpp`: **398 -> 110** lines; `.cpp`: **1166 -> 1166**;
  `.h`: **473 -> 455**. New core files are below 500 lines.
- `git diff --check`: clean. Existing tests and golden files are unchanged.

Unverified native checks:

- `ChartGoldenTests` failed: `metal backend: no MTLDevice (Metal unavailable)`.
- `ChartUiTests` aborted creating QRhi/Metal after the CPU toolbar cases.
- `UgrGpuTests` returned success but **76 GPU cases skipped**, 24 CPU cases passed.
- `UgrGpuServiceTeardownGuardMalloc`: **1 skipped**, 0 passed.
- `HeatmapSettingsUiTests`: **9 GPU cases skipped**, 34 cases passed.

The operator must run the Metal gate natively with `SENTINEL_REQUIRE_GPU=1`,
including the golden, chart UI, UGR GPU/glide and skipped UI/teardown checks.
No screenshot or live-provider result is claimed. Only synthetic fixtures ran.
