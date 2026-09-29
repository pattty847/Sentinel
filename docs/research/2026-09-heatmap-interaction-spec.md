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
     per pixel. Switching back to Auto resumes the Auto rule from the current zoom. The Manual
     tick is remembered per symbol and timeframe (BTC 1m can keep $1 while BTC 1h keeps $25), so
     switching feels like returning to a workspace.
   - **Manual never coarsens silently.** If a locked tick cannot be built from some visible
     history (for example $1 over older $10-grid data), keep the locked tick and draw those
     regions as unavailable (veil) with an explicit resolution indicator. Only Auto may skip to
     the finest compatible preset.
3. **Price-grid anchoring.** Tick bins are anchored to the asset's absolute price lattice, never
   to the viewport. Panning, zooming, chunk boundaries and reloads cannot shift bin boundaries: a
   given (tick, price) always maps to the same row. (Same for time: columns are anchored to UTC
   epoch multiples of the timeframe.)
4. **Presets.** `{1, 2, 2.5, 5} x 10^k` in the asset's price units, offered only when they are
   multiples of `commonTick()` of the data in view (so older BTC deep history recorded on a $10
   grid offers $10, $20, $50 ...; recent near data offers $1, $2, $5 ...). A preset the visible
   data cannot build is not offered (Manual) or skipped (Auto). No arbitrary floats. Per-asset
   defaults for Manual (for example a sensible default tick per timeframe) are presets from the
   same ladder.
   *Clarification (slice T, pending owner confirmation):* "not offered" conflicts with rule 2's
   veil case ($1 over older $10 history). The lab offers Manual every preset that at least part
   of the loaded data can build; columns in view that cannot build it veil and the resolution
   indicator names them.
5. **Cell meaning.** A cell is time-weighted liquidity over the displayed timeframe, summed across
   the constituent native price rows that make up the selected tick. Only observed, valid source
   intervals contribute; incomplete required coverage is never silently treated as zero. Coarser
   ticks never draw a single constituent row instead.
   Unknown coverage draws the veil; not-yet-loaded data draws the loading hatch; time before the
   oldest data draws nothing.
6. **Re-bin triggers.** Tick change (Auto crossing or Manual choice), timeframe change, new or
   revised chunks that intersect the prepared/renderable region (background prefetch outside it
   never triggers GPU work), source generation or config change, and the view leaving the
   prepared region at the same tick. Pan inside the prepared region is a pure GPU translation.
7. **No freeze.** Once the required source data is local, a re-bin is a GPU pass (S4: about
   0.1-2 ms), so a tick change appears in the next frame. The active source keeps drawing until a
   replacement is ready; the chart is never blanked, stretched or rebuilt from scratch.
8. **Transitions.** Hard switch by default. A short crossfade between the old and new tick grids
   is added only if lab experiment E2 shows the hard switch is visually jarring.
9. **Axis drags and wheel follow the same limits.** Stretching an axis can never exceed the
   clamps in rules 1 and 2 (the legacy path allowed it; that inconsistency goes away).

## Lab experiments (sentinel-lab, real recorded data, before the main-chart switch)

- **E1 Auto tick feel:** wheel zoom in and out across several tick boundaries with `h` in
  {0, 0.15, 0.25, 0.4}; the owner picks the value that never twitches but still refines promptly.
  Report re-bin time per tick change.
- **E2 Transition:** hard switch vs 100-200 ms crossfade at a tick change; the owner decides
  whether a crossfade is needed.
- **E3 Manual mode:** lock $1, $10, $100 on BTC; confirm zoom only scales, the 1 row/px clamp, the
  per-symbol/timeframe memory, and that $1 over older $10-grid history shows the veil and the
  resolution indicator instead of coarsening.
- Each experiment ships a `--screenshot` or recorded sequence and the lab debug panel shows mode,
  tick, `h`, and last re-bin ms.

## Not in scope here

Storage format (raw L2 capture, keyframes, compression: `2026-09-storage-pyramid.md`), wire
protocol (S3), chunk controller and caching (B1, S5), and multi-chart budgets (B1).

In short: Auto adapts, Manual obeys, zoom never changes the timeframe, price grids never move
under the data, and unsupported history is never fabricated.
