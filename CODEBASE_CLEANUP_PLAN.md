# Sentinel Codebase Cleanup Plan

**Priority: Fix the "odd chunks" and timeframe issues FIRST, then systematically clean up the codebase.**

## 🔥 CRITICAL SEVERITY - Fix Immediately (Blocking Development)

### 1. Price Binning/Rendering Mismatch (ROOT CAUSE of "odd chunks")
**Issue**: `DataProcessor::timeSliceToScreenRect()` uses dynamic `getPriceResolution()` for rect height while slices retain original `tickSize` for binning.
- **File**: `libs/gui/render/DataProcessor.cpp:438-476`
- **Fix**: Use `slice.tickSize` instead of `getPriceResolution()` for vertical rect calculation
- **Impact**: Visual chunks will align with actual data bins

### 2. Data Race on Visible Cells
**Issue**: `m_visibleCells` accessed across threads without locking
- **Files**: 
  - `libs/gui/UnifiedGridRenderer.cpp:175-179` (reader)
  - `libs/gui/render/DataProcessor.hpp:76` (writer)
- **Fix**: Add mutex-protected copy accessor for thread-safe cell transfer
- **Impact**: Prevents crashes and visual corruption during rendering

### 3. Time Source Inconsistency (Timeframe Misalignment)
**Issue**: LTSE uses `now()` instead of exchange timestamps, causing viewport/slice misalignment
- **Files**:
  - `libs/core/LiquidityTimeSeriesEngine.cpp:72-79` (using `now()`)
  - `libs/gui/render/DataProcessor.cpp:203-208` (using `now()`)
  - `libs/core/MarketDataCore.cpp:492-506` (has exchange timestamp)
- **Fix**: Propagate exchange timestamps through the entire pipeline
- **Impact**: Time-based queries will work correctly

### 4. Quantization Drift (Price Bin Shifting)
**Issue**: `std::round()` causes price bins to drift between frames
- **Files**:
  - `libs/core/LiquidityTimeSeriesEngine.cpp:535-537`
  - `libs/core/LiquidityTimeSeriesEngine.h:100-102`
- **Fix**: Use side-aware floor/ceil (floor for bids, ceil for asks)
- **Impact**: Consistent price level alignment

## 🚨 HIGH SEVERITY - Fix Next (Architecture Integrity)

### 5. Multiple Timeframe Systems Conflicting
**Issue**: 7 timeframes maintained simultaneously + manual override + auto-suggestion creating conflicts
- **Files**: 
  - `libs/gui/render/DataProcessor.cpp:49-53` (global timeframe setup)
  - `libs/gui/render/DataProcessor.cpp:280-296` (auto/manual conflict)
- **Fix**: Single source of truth for timeframe management
- **Impact**: Eliminates timeframe switching confusion

### 6. Mixed Price Resolution Systems
**Issue**: Historical slices keep old `tickSize` when resolution changes dynamically
- **Files**:
  - `libs/gui/UnifiedGridRenderer.cpp:136-144` (dynamic resolution change)
  - `libs/core/LiquidityTimeSeriesEngine.cpp:341-349` (slice creation with fixed tickSize)
- **Fix**: Either freeze LTSE resolution or rebuild slices on change
- **Impact**: Consistent visual granularity

### 7. Axis/Bucket Misalignment
**Issue**: Price axis uses "nice" steps independent of LTSE resolution
- **File**: `libs/gui/models/PriceAxisModel.cpp:98-125`
- **Fix**: Snap axis ticks to current slice tickSize
- **Impact**: Grid lines align with data buckets

## 🔧 MEDIUM SEVERITY - Clean Architecture (Remove Confusion)

### 8. Dual Zoom Implementations
**Issue**: Two different `handleZoom` methods with conflicting logic
- **File**: `libs/gui/render/GridViewState.cpp:98-128` vs `158-205`
- **Fix**: Consolidate into single, correct implementation
- **Impact**: Consistent zoom behavior

### 9. Unsafe Index Math in Dense→Sparse Conversion
**Issue**: Mixed double/size_t math with potential bounds violations
- **File**: `libs/gui/render/DataProcessor.cpp:220-236`
- **Fix**: Explicit floor/ceil with integer bounds checking
- **Impact**: Prevents array access crashes

### 10. Intensity Scaling Not Normalized
**Issue**: Cell intensity uses raw liquidity/1000 regardless of timeframe/bucket size
- **File**: `libs/gui/render/DataProcessor.cpp:397-405`
- **Fix**: Normalize by time duration and price bucket size
- **Impact**: Consistent visual intensity across zoom levels

## 📚 LOW SEVERITY - Cleanup and Organization

### 11. Delete Dead Files and Directories
```bash
# Keep OLD_GRID_RENDERER.txt for now (reference during refactoring)
rm -rf tests/old_tests/
rm -rf docs/archive/ 
rm libs/gui/CMakeCache.txt
rm docs/LIVE_ARCHITECTURE.md
rm docs/V2_RENDERING_ARCHITECTURE.md  
rm docs/SYSTEM_ARCHITECTURE.md
```

### 12. Cleanup Partial Refactor Markers
- 78 "PHASE" comments indicating incomplete work
- 6 TODO/FIXME markers
- Remove conflicting documentation

### 13. File Size Violations (Address AFTER fixing critical issues)
```
MarketDataCore.cpp:      608 LOC → Extract WebSocket handling
UnifiedGridRenderer.cpp: 547 LOC → Extract business logic to core
DataProcessor.cpp:       536 LOC → Extract cell generation
LiquidityTimeSeriesEngine.cpp: 536 LOC → Extract aggregation logic
SentinelMonitor.cpp:     508 LOC → Extract monitoring strategies
```

## 🎯 EXECUTION STRATEGY

### Phase 1: Emergency Fixes (1-2 days)
1. Fix price rect calculation using `slice.tickSize`
2. Add thread-safe cell accessor
3. Propagate exchange timestamps 
4. Fix quantization rounding

### Phase 2: Architecture Consolidation (2-3 days)  
5. Unify timeframe management
6. Resolve price resolution conflicts
7. Align axis with data buckets
8. Consolidate zoom implementations

### Phase 3: Code Quality (1-2 days)
9. Fix unsafe math operations
10. Normalize intensity scaling
11. Remove dead files
12. Clean up phase markers

### Phase 4: Refactoring for LOC Compliance (Later)
13. Extract oversized files to meet 300 LOC limit

## 🧪 VALIDATION AFTER EACH PHASE

```bash
# Build and test
cmake --build --preset mac-clang
cd build-mac-clang && ctest --output-on-failure

# Visual verification  
./apps/sentinel_gui/sentinel_gui
# Check: No more "odd chunks", smooth zoom, aligned price levels
```

## 📋 SUCCESS CRITERIA

- ✅ Price chunks align visually with data bins
- ✅ Smooth zoom without visual artifacts  
- ✅ Consistent timeframe behavior
- ✅ No crashes during live data updates
- ✅ Clean, professional codebase architecture

---

**Note**: Keep OLD_GRID_RENDERER.txt as reference until refactoring is complete and proven stable.