# Multi-Mode Chart Architecture - Post-Codex Integration Fixes

**Date**: 2025-06-28  
**Status**: 70% Complete - Scaffolding Done, Need Architecture Cleanup  
**Priority**: HIGH - Core functionality ready, needs focused refactoring  

## 🎯 **CONTEXT: What Just Happened**

A Codex agent successfully implemented the multi-mode chart architecture from our detailed plan. The **structural changes are excellent** but it didn't fully commit to the "dumb renderer" architectural principle. We now have a hybrid system that needs cleanup.

### ✅ **What Codex Got RIGHT (Don't Touch These)**
- **New High-Frequency Timeframes**: 100ms, 500ms, 1sec candles (perfect for 16ms rendering)
- **GPUDataAdapter Candle Batching**: Proper single-source-of-truth aggregation
- **ChartModeController**: Clean mode switching scaffolding
- **Signal Routing**: Correct connections in MainWindow
- **Integration Test**: Comprehensive test with compile-time assertions
- **CandleLOD Removal**: Correctly removed duplicate aggregation from CandlestickBatched

### 🚨 **What Needs Fixing (Your Mission)**
The core issue: **CandlestickBatched is still doing data processing when it should be a pure renderer**.

---

## 🚨 **CRITICAL FIXES REQUIRED**

### **Fix #1: Clean Up CandlestickBatched Interface**

**Problem**: The class still has legacy trade processing methods that violate the new architecture.

**Files to Modify**: 
- `libs/gui/candlestickbatched.h` (lines ~33-50)
- `libs/gui/candlestickbatched.cpp` (constructor and related methods)

**REMOVE these methods entirely:**
```cpp
// ❌ DELETE FROM HEADER (.h file):
Q_INVOKABLE void addTrade(const Trade& trade);
void onTradeReceived(const Trade& trade);
void onTradesReady(const std::vector<Trade>& trades);
void processPendingUpdates();  // private slot
```

**REMOVE these member variables:**
```cpp
// ❌ DELETE FROM HEADER (.h file):
QTimer* m_updateTimer;
std::mutex m_pendingTradesMutex;
std::vector<Trade> m_pendingTrades;
bool m_hasPendingUpdates;
```

**REMOVE corresponding implementations from .cpp file** (search for these method names)

### **Fix #2: Implement Missing onCandlesReady Method**

**Problem**: The header declares `onCandlesReady` but there's no implementation in the .cpp file.

**File**: `libs/gui/candlestickbatched.cpp`

**ADD this method** (around line 35-40, after constructor):
```cpp
void CandlestickBatched::onCandlesReady(const std::vector<CandleUpdate>& candles) {
    if (candles.empty()) return;
    
    // Store only the latest candle updates for rendering (no persistent storage)
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_latestCandleUpdates = candles;  // Replace with new data
    }
    
    // Trigger GPU re-render
    m_geometryDirty.store(true);
    update();
    
    qDebug() << "🕯️ CANDLES READY:" << candles.size() << "updates received for rendering";
}
```

**ALSO ADD this member to header** (`candlestickbatched.h`):
```cpp
// Add to private section:
std::vector<CandleUpdate> m_latestCandleUpdates;  // Temporary for rendering only
```

**ALSO ADD forward declaration** at top of header:
```cpp
struct CandleUpdate;  // Add this line near the top
```

### **Fix #3: Update updatePaintNode to Use New Data Flow**

**Problem**: The rendering method still tries to access old `m_candleLOD` data that no longer exists.

**File**: `libs/gui/candlestickbatched.cpp` (around line 120-130)

**FIND this line:**
```cpp
const auto& candles = m_candleLOD->getCandlesForTimeFrame(activeTimeFrame);
```

**REPLACE with:**
```cpp
// Filter latest updates for the active timeframe
std::vector<OHLC> relevantCandles;
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    for (const auto& update : m_latestCandleUpdates) {
        if (update.timeframe == activeTimeFrame) {
            relevantCandles.push_back(update.candle);
        }
    }
}
const auto& candles = relevantCandles;
```

### **Fix #4: Update Class Documentation**

**File**: `libs/gui/candlestickbatched.h` (top comment, around line 15)

**REPLACE the class comment:**
```cpp
// 🕯️ DUMB CANDLESTICK RENDERER: Receives pre-aggregated candles from GPUDataAdapter
// Pure GPU rendering component - NO business logic or data aggregation
// Data Flow: GPUDataAdapter → candlesReady signal → onCandlesReady() → GPU render
```

---

## 🧪 **VERIFICATION STEPS**

### **Step 1: Compilation Test**
```bash
cd /path/to/Sentinel
cmake --build build
```
**Expected**: Should compile without errors. The compile-time assertion in the test should PASS.

### **Step 2: Integration Test**
```bash
cd build && ctest -R MultiModeArchitecture --verbose
```
**Expected**: Test should pass and show "candlesReady signal emitted"

### **Step 3: Architecture Verification**
After fixes, `CandlestickBatched` should have:
- ✅ **ONLY** `onCandlesReady()` as data input method
- ✅ **NO** trade processing or aggregation logic  
- ✅ **NO** persistent candle storage (only temporary for rendering)
- ✅ Pure GPU rendering functionality

---

## 🎯 **SUCCESS CRITERIA**

**You'll know it's working when:**
1. **Compile-time assertion passes**: The test assertion `!has_addTrade<CandlestickBatched>` compiles successfully
2. **Clean interface**: CandlestickBatched has exactly ONE data input method
3. **No data duplication**: GPUDataAdapter is the single source of truth for candle aggregation
4. **Test passes**: Integration test shows proper batching behavior

**Estimated Time**: 1-2 hours of focused refactoring

---

## 📋 **CONTEXT FOR NEXT AGENT**

**Current Architecture State:**
- ✅ GPUDataAdapter: Aggregates trades → emits candlesReady every 16ms
- ✅ ChartModeController: Mode switching infrastructure ready
- 🔧 CandlestickBatched: Needs cleanup to become pure renderer
- ✅ MainWindow: Proper signal connections established
- ✅ Tests: Framework ready, assertions will validate fixes

**Key Files Modified by Codex:**
- `libs/gui/candlelod.{h,cpp}` - Enhanced with high-freq timeframes
- `libs/gui/gpudataadapter.{h,cpp}` - Added candle batching pipeline  
- `libs/gui/candlestickbatched.{h,cpp}` - Partially refactored (needs completion)
- `libs/gui/chartmodecontroller.{h,cpp}` - New mode switching system
- `tests/test_multi_mode_architecture.cpp` - Integration test with assertions

**The Big Win**: We solved the fundamental timing mismatch (1-minute candles vs 16ms rendering) by adding 100ms/500ms timeframes that align perfectly with the rendering frequency! 