<!-- 8e0b2c4b-a2a9-4db1-924b-4d89da88b14c f1d13d15-7ed9-49f9-a405-1733511adfe2 -->
# DOD Heatmap Optimization Plan

## Problem Summary

Current render path destroys and recreates QSG nodes every frame, allocates temporary vectors per chunk, and constructs QColor objects per cell. These patterns waste CPU cycles on allocation/deallocation overhead instead of actual rendering work.

## Target Files

- `libs/gui/render/GridSceneNode.cpp` - node lifecycle management
- `libs/gui/render/GridSceneNode.hpp` - node storage
- `libs/gui/render/strategies/HeatmapStrategy.cpp` - geometry generation
- `libs/gui/render/strategies/HeatmapStrategy.hpp` - strategy interface
- `libs/gui/render/IRenderStrategy.hpp` - interface changes (if needed)

## Implementation

### Task 1: QSG Node Reuse in GridSceneNode

Current code destroys and recreates heatmap node every frame:

```cpp
// GridSceneNode.cpp:30-38
if (m_heatmapNode) {
    removeChildNode(m_heatmapNode);
    delete m_heatmapNode;
}
m_heatmapNode = heatmapStrategy->buildNode(dataAccessor);
```

Change to update-in-place pattern:

- Keep `m_heatmapNode` alive across frames
- Add `IRenderStrategy::updateNode(QSGNode*, IDataAccessor*)` method
- Only rebuild when node is null or structure changes (chunk count)
- Reuse geometry buffers when vertex count fits

### Task 2: Geometry Buffer Reuse in HeatmapStrategy

Current code allocates new `QSGGeometry` per chunk per frame:

```cpp
// HeatmapStrategy.cpp:96-105
auto* node = new QSGGeometryNode;
auto* geometry = new QSGGeometry(..., vertexCount);
```

Change to reuse existing geometry:

- Check if existing node has sufficient vertex capacity
- If capacity fits, reuse buffer and just update vertices
- If not, reallocate (rare case)
- Track chunk nodes in a pool instead of creating fresh

### Task 3: Eliminate chunkCells Vector

Current code allocates a vector per chunk:

```cpp
// HeatmapStrategy.cpp:80-88
std::vector<const CellInstance*> chunkCells;
chunkCells.reserve(targetCells);
for (...) { chunkCells.push_back(&c); }
```

Change to direct iteration:

- Pre-count cells passing filter (already done)
- Iterate cells directly while writing vertices
- Use index arithmetic, no intermediate collection

### Task 4: Inline Color Calculation

Current code creates QColor per cell:

```cpp
// HeatmapStrategy.cpp:115-119
QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
const int r = color.red();
```

Change to inline RGBA calculation:

- Compute r/g/b/a directly as integers
- Avoid QColor construction overhead
- Keep intensity-to-color formula identical

## Validation

1. Visual: Heatmap renders identically before/after
2. Performance: `contentUs` reduced by 30-50% for same cell count
3. Stability: No crashes on zoom/pan/resize cycles
4. Memory: Steady-state allocations reduced (no per-frame churn)

## Risk Mitigation

- Gate behind feature flag initially (`USE_HEATMAP_NODE_REUSE`)
- Compare logs before/after for same viewport
- Test layer toggle (hide/show heatmap) to ensure cleanup works

### To-dos

- [ ] Implement QSG node reuse in GridSceneNode - keep nodes alive, update in place
- [ ] Add geometry buffer reuse in HeatmapStrategy - reallocate only when needed
- [ ] Remove chunkCells vector - iterate cells directly while writing vertices
- [ ] Inline color calculation - compute RGBA directly without QColor