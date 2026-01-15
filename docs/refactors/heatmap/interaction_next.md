## Heatmap Interaction - Next Goals

### Current behavior
- Auto-scroll keeps "now" pinned to the right edge.
- Horizontal drag is sluggish; often requires zooming before drag takes effect.
- Vertical drag works but horizontal drag feels like it fights appends.
- Auto-scroll resumes and can re-pin after drag release.

### Hypothesis
- The render loop continues to advance the live head while the pan visual offset
  is being applied, so drag feels delayed or overridden.
- Horizontal drag uses a lag offset, but capture timing is too late (on release)
  and the offset is too small to feel immediate.

### Desired behavior (spec)
1) Immediate drag feedback on mouse-down:
   - Heatmap should move with the cursor from the first pixel.
2) Auto-scroll pause on drag:
   - Drag should not fight appends while the mouse is held.
3) Unpin from right edge:
   - User can drag "now" away from the right edge and it stays there until
     auto is explicitly re-enabled.

### Candidate fixes
- Capture time-lag offset on pan start (not release).
- While dragging, ignore auto-scroll viewport updates entirely.
- Add a "follow live" toggle:
  - Auto ON: snap to live head with configurable padding.
  - Auto OFF: no snapping; drag is pure viewport control.
- Apply pan visual offset to axis labels for instant alignment during drag.

### Key files to load next session
- `libs/gui/UnifiedGridRenderer.cpp`
- `libs/gui/render/GridViewState.cpp`
- `libs/gui/qml/DepthChartView.qml`

### Related runtime flags
- `SENTINEL_GPU_HEATMAP=1`
- `SENTINEL_HEATMAP_TF=100`
