# Heatmap Refactor Plan (GPU-First)

Goal: replace CPU-per-cell geometry rendering with a GPU-resident heatmap that is shaded in a single draw call and supports large grids via tiling and dirty updates.

## Guiding Principles

- CPU should not loop per-cell during draw. CPU only uploads data and sets uniforms.
- GPU shades the heatmap from a texture or buffer.
- Zoom/pan changes uniforms, not geometry.
- Data pipeline must be thread-safe and deterministic.

## Phase 0: Correctness Stabilization (Blocking)

1) **Fix cross-thread GUI access**
- `GridViewState` must never be accessed from the worker thread.
- Replace direct use with immutable `ViewportSnapshot` passed into `DataProcessor` via queued signal.

2) **Make remote data thread-safe**
- Guard `RemoteGridDataSource` book map and `LiveOrderBook` with a mutex, or move book updates to the worker thread and provide snapshots for the GUI.

3) **Serialize websocket writes**
- Use a strand or single-threaded write loop for client writes.
- Ensure server session access to `subscriptions_` is serialized (strand or mutex).

4) **Extend snapshot protocol**
- Add `minPrice`, `maxPrice`, `tickSize`, `timestamp` fields to snapshot payloads.
- Use them to initialize client `LiveOrderBook` correctly.

5) **Decide gap policy**
- Decide whether to carry-forward missing 100ms buckets or show honest gaps.
- Implement consistently in `captureOrderBookSnapshot`.

6) **Fix default range initialization**
- Remove hardcoded `75000..125000` / `0.01` defaults.
- Derive range from snapshot + config; send via protocol to client.

## Phase 1: GPU Heatmap Prototype (Single Texture, Single Quad)

1) **Data layout**
- Create a fixed-size 2D grid (time x price) on CPU as a ring buffer.
- Store a single-channel intensity (R8 or R16). Color mapping happens in shader.
 - Keep layout contiguous and row-major; index = (time_idx * price_bins + price_idx).

2) **Dirty tracking**
- Maintain dirty rectangles (or dirty tiles) for updates.
- Upload only dirty regions to GPU via QRhi/QSG texture updates.
 - Cap per-frame upload budget to avoid stalls.

3) **Render node**
- Implement a new heatmap renderer (QSGRenderNode or custom QSGGeometryNode) that draws a single quad.
- Fragment shader samples the heatmap texture and LUT/palette.

4) **Uniforms**
- Provide viewport mapping parameters (time/price extents, zoom, pan).
- Shader converts screen coords to grid coords and samples.
 - Optional: add a switch for raw sampling vs mip/prefilter.

5) **Switch control**
- Keep the old heatmap behind a flag; default to new GPU path once stable.

Deliverable: a working heatmap where CPU cost is constant with cell count.

## Phase 2: Tiles + History Scale

1) **Tiling strategy**
- Split the grid into fixed tiles (e.g., 256x256).
- Only keep visible tiles resident in GPU memory.
 - Store tiles in a ring per timeframe (100ms live, 1s persisted).

2) **Streaming**
- For long history, stream tiles from disk or server.
- Maintain an LRU cache for tile residency.

3) **Multi-resolution (optional)**
- Add pre-aggregated levels (mip pyramid or multi-TF slices).
- When zoomed out, sample lower-res tiles.

Deliverable: heatmap supports very large history without VRAM blowout.

## Phase 3: Integration + Cleanup

1) **Remove old CPU heatmap strategy**
- Delete `HeatmapStrategy` and related geometry code.
- Remove any CPU-only per-cell path.

2) **Refactor DataProcessor output**
- Output dirty rectangles/tiles and a ring buffer write cursor instead of full `CellInstance` vectors.
 - Move any per-cell work into the aggregation step, not the render step.

3) **Tests**
- Add a minimal integration test: server -> client -> snapshot -> GPU buffer update.
- Add a synthetic data driver to validate tile updates and shader mapping.
 - Add a stress test for large tile counts (no frame stalls).

## Risks and Mitigations

- **Qt Quick integration complexity**: use QSGRenderNode + QRhi (Qt 6) to avoid legacy GL calls.
- **Latency spikes on large uploads**: limit per-frame uploads and spread over frames.
- **Visual artifacts during zoom**: ensure stable mapping math and avoid precision loss.

## Immediate Next Tasks

1) Design `ViewportSnapshot` (thread-safe) and update `DataProcessor` to consume it.
2) Extend snapshot protocol to include `minPrice`, `maxPrice`, `tickSize`, `timestamp`.
3) Implement a minimal GPU heatmap node with a single texture + shader + quad.
4) Wire dirty update uploads from worker -> render thread.
5) Simplify LTSE snapshot format (remove map-based snapshots where possible).

## Success Criteria

- CPU cost does not scale with cell count.
- Draw calls are constant (one quad or one per tile).
- Can hold and display tens of millions of cells in GPU memory without stutter.
