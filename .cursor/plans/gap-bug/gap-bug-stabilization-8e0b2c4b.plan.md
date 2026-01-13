<!-- 8e0b2c4b-a2a9-4db1-924b-4d89da88b14c c056b54f-9ce2-4fae-afbe-63c9b9c82653 -->
# Gap Bug Stabilization Plan

## Problem Summary

Vertical gaps appear in the heatmap rendering. Root causes identified:

1. **Append-mode bug**: `DataProcessor::updateVisibleCells()` tracks processed slices by `(startTime, endTime)` only. If a slice receives additional data after being processed, it is never reprocessed, leaving gaps where new liquidity data exists but is not rendered.

2. **Current slice edge case**: The "current" (in-progress) slice in `LiquidityTimeSeriesEngine` is mutable and constantly updated, but once processed by append-mode, it won't be reprocessed even as new snapshots arrive.

## Key Files

- `libs/gui/render/DataProcessor.cpp` - append-mode logic (lines 486-539)
- `libs/gui/render/DataProcessor.hpp` - `SliceTimeRange` tracking (lines 156-171)
- `libs/core/LiquidityTimeSeriesEngine.cpp` - slice management
- `libs/core/LiquidityTimeSeriesEngine.h` - `LiquidityTimeSlice` struct

## Implementation

### Task 1: Add Version Tracking to Slices

Add a `dataVersion` field to `LiquidityTimeSlice` that increments when metrics change:

```cpp
// In LiquidityTimeSeriesEngine.h, inside LiquidityTimeSlice struct
uint64_t dataVersion = 0;  // Incremented when any metrics change
```

Increment this in `addSnapshotToSlice()` after updating metrics.

### Task 2: Extend Processed Slice Tracking

Modify `DataProcessor`'s tracking to include the data version:

```cpp
// In DataProcessor.hpp
struct SliceTimeRange {
    int64_t startTime;
    int64_t endTime;
    uint64_t dataVersion;  // NEW: track data version
    
    bool operator==(const SliceTimeRange& other) const {
        return startTime == other.startTime 
            && endTime == other.endTime
            && dataVersion == other.dataVersion;
    }
};

struct SliceTimeRangeHash {
    size_t operator()(const SliceTimeRange& range) const {
        return std::hash<int64_t>()(range.startTime) 
             ^ (std::hash<int64_t>()(range.endTime) << 1)
             ^ (std::hash<uint64_t>()(range.dataVersion) << 2);
    }
};
```

### Task 3: Update Append-Mode Logic

In `DataProcessor::updateVisibleCells()`, modify the append-mode section to check version:

```cpp
// Around line 500-510 in DataProcessor.cpp
SliceTimeRange range{slice->startTime_ms, slice->endTime_ms, slice->dataVersion};

if (m_processedTimeRanges.find(range) == m_processedTimeRanges.end()) {
    // Remove old version of this time range if exists (data changed)
    for (auto it = m_processedTimeRanges.begin(); it != m_processedTimeRanges.end(); ) {
        if (it->startTime == range.startTime && it->endTime == range.endTime) {
            it = m_processedTimeRanges.erase(it);
        } else {
            ++it;
        }
    }
    
    ++processedSlices;
    createCellsFromLiquiditySlice(*slice);
    m_processedTimeRanges.insert(range);
}
```

### Task 4: Handle Stale Cells on Slice Update

When a slice is reprocessed due to version change, we need to remove its old cells first. Add a helper to remove cells by time range before re-creating them:

```cpp
// New helper in DataProcessor
void DataProcessor::removeCellsForTimeRange(int64_t startTime, int64_t endTime) {
    m_visibleCells.erase(
        std::remove_if(m_visibleCells.begin(), m_visibleCells.end(),
            [startTime, endTime](const CellInstance& cell) {
                return cell.timeStart_ms == startTime && cell.timeEnd_ms == endTime;
            }),
        m_visibleCells.end()
    );
}
```

Call this before `createCellsFromLiquiditySlice()` when reprocessing a changed slice.

### Task 5: Add Diagnostic Logging

Add temporary debug logging to confirm the fix works:

```cpp
// In append-mode section
if (oldVersionFound) {
    sLog_Render("SLICE REPROCESS: time=[" << range.startTime << "-" << range.endTime 
                << "] version " << oldVersion << " -> " << range.dataVersion);
}
```

### Task 6: Capture Baseline Performance

After confirming gaps are fixed:

- Run with typical BTC-USD data
- Capture `UGR paint: total/cache/content/cells` logs
- Document in `docs/refactors/data_pipeline_dod/baseline_metrics.md`

## Validation

1. Visual check: No vertical gaps in heatmap during normal operation
2. Log check: `SLICE REPROCESS` logs appear when data updates arrive for existing slices
3. Cell distribution: `CELL DISTRIBUTION` diagnostic shows continuous time coverage
4. No performance regression: `cacheUs` should not significantly increase (small increase acceptable due to version checking)

## Out of Scope

- DOD optimizations (Phase 5-9)
- QSG node reuse
- CellInstance struct changes
- Documentation updates (separate phase)

### To-dos

- [ ] Add dataVersion field to LiquidityTimeSlice and increment on metric changes
- [ ] Extend SliceTimeRange to include dataVersion in hash and equality
- [ ] Update append-mode logic to detect and handle version changes
- [ ] Add helper to remove stale cells before reprocessing a slice
- [ ] Add diagnostic logging to confirm slice reprocessing works
- [ ] Capture and document baseline performance metrics after fix