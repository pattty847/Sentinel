# Heatmap interaction spec (owner-approved 2026-09-29)

How the heatmap responds to zoom, pan, tick and timeframe changes. This spec is the product
contract for the GPU heatmap path (integration plan slices T, B1, S5-S8). Where it says
"decided in the lab", the lab experiment below decides, not an agent.

## Terms

- **Source:** stored liquidity data (today HMC2 near $1 / deep $5-$10 grids at 1m and 1h; later
  raw pristine L2 with 1s / 1m / 1h serving levels).
- **Timeframe:** the width of one heatmap column (1m, 5m, 15m, 1h, 4h, 1D, or a custom
  whole-minute value). Chosen by the user only.
- **Tick:** the height of one heatmap row in price units. Chosen by Auto or by the user.

## Rules

1. **A column is exactly the timeframe.** Zoom and pan never change it. There is no
   auto-timeframe. Time zoom-out stops at one column per screen pixel; to see further back,
   pan or pick a longer timeframe (TradingView model).
2. **Tick modes.**
   - **Auto (default).** Tick = the smallest preset whose rows are at least `minRowPx` tall
     (2 px today; tune by feel in the lab). Zooming in reaches the finest preset the data
     supports, e.g. BTC $1. Hysteresis: step finer only when the finer preset would be at least
     `minRowPx x (1 + h)` tall; step coarser only when the current rows fall below
     `minRowPx x (1 - h)`. `h` starts at 0.25 and is tuned in the lab (experiment E1).
   - **Manual.** The user locks a preset. Zoom only scales rows. Price zoom-out stops at one row
     per pixel. Switching back to Auto resumes the Auto rule from the current zoom.
3. **Presets.** `{1, 2, 2.5, 5} x 10^k` in the asset's price units, offered only when they are
   multiples of `commonTick()` of the data in view (so older BTC deep history recorded on a $10
   grid offers $10, $20, $50 ...; recent near data offers $1, $2, $5 ...). A preset the visible
   data cannot build is not offered (Manual) or skipped (Auto). No arbitrary floats. Per-asset
   defaults for Manual (for example a sensible default tick per timeframe) are presets from the
   same ladder.
4. **Cell meaning.** A cell is the summed time-weighted liquidity of every constituent row and
   minute (the TWAP aggregate). Coarser ticks never draw a single constituent row instead.
   Unknown coverage draws the veil; not-yet-loaded data draws the loading hatch; time before the
   oldest data draws nothing.
5. **Re-bin triggers.** Tick change (Auto crossing or Manual choice), timeframe change, new or
   revised source data, new chunks, source generation or config change, and the view leaving the
   prepared region at the same tick. Pan inside the prepared region is a pure GPU translation.
6. **No freeze.** A re-bin is a GPU pass over data the client already holds (S4: about 0.1-2 ms),
   so a tick change appears in the next frame. The active source keeps drawing until a
   replacement is ready; the chart is never blanked, stretched or rebuilt from scratch.
7. **Transitions.** Hard switch by default. A short crossfade between the old and new tick grids
   is added only if lab experiment E2 shows the hard switch is visually jarring.
8. **Axis drags and wheel follow the same limits.** Stretching an axis can never exceed the
   clamps in rules 1 and 2 (the legacy path allowed it; that inconsistency goes away).

## Lab experiments (sentinel-lab, real recorded data, before the main-chart switch)

- **E1 Auto tick feel:** wheel zoom in and out across several tick boundaries with `h` in
  {0, 0.15, 0.25, 0.4}; the owner picks the value that never twitches but still refines promptly.
  Report re-bin time per tick change.
- **E2 Transition:** hard switch vs 100-200 ms crossfade at a tick change; the owner decides
  whether a crossfade is needed.
- **E3 Manual mode:** lock $1, $10, $100 on BTC; confirm zoom only scales and the 1 row/px clamp.
- Each experiment ships a `--screenshot` or recorded sequence and the lab debug panel shows mode,
  tick, `h`, and last re-bin ms.

## Not in scope here

Storage format (raw L2 capture, keyframes, compression: `2026-09-storage-pyramid.md`), wire
protocol (S3), chunk controller and caching (B1, S5), and multi-chart budgets (B1).
