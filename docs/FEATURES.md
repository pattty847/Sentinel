# Completed Features

## 2026-02-01 — Axis performance tuning

### Performance evolution (profiling runs)

- Run 1 (baseline): 14.2s total — “creation bottleneck,” ~26k Text/Item creations. 10 FPS pan and zoom due to model churn for axis labels. 
  Fixed: Identified `Repeater` churn from `beginResetModel()` + high tick density as primary issue.
- Run 2: 7.09s total — creation counts cut to ~10k, big allocator relief. 
  Fixed: Reduced label density and stopped full model resets on every pan/zoom.
- Run 3: 5.32s total — fixed-capacity labels; creation drops to ~64 items; binding overhead becomes dominant.
  Fixed: Fixed-capacity axis model + reuse of label slots; delegates stay alive.
- Run 5 (after pan update): ~125 FPS with deferred axis updates.
  Fixed: Moved pan offset out of per-label bindings; axis updates deferred to viewport commits.
- Run 6 (live updates during drag): ~110 FPS with dynamic axis updates under aggressive pan/zoom.
  Fixed: Axis models recompute during drag using visual pan offset; time axis nice ticks + hysteresis.

### Key changes shipped

- Fixed-capacity axis models to stop QML object churn.
- Nice ticks on price + time axes with hysteresis.
- Live axis updates during drag using visual pan ranges.
- Role-scoped `dataChanged` to cut binding overhead.
- Moved per-label pan math out of QML bindings into model space.
