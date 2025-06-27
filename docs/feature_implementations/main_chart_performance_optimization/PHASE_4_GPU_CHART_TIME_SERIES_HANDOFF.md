# 🚀 **GPU Chart Time-Series Handoff Document**

## **CURRENT STATUS: COORDINATE SYSTEM RECONSTRUCTION NEEDED**

### **🎯 GOAL: Time-Series Trading Terminal**
The user wants to replicate the attached trading terminal image:
- **X-axis**: Time progression (left=past, right=present)  
- **Y-axis**: Price levels
- **Trades**: Small dots plotted at (timestamp, price) coordinates
- **Order Book**: Vertical bars at each time slice showing bid/ask levels
- **Accumulative**: Keep building chart over time, don't replace data

### **🔥 CRITICAL ISSUES TO FIX**

#### **1. Multiple Conflicting Coordinate Systems**
**Problem**: Currently has test mode vs real data mode with different coordinate mappings
**Solution**: Remove all test mode code, use single time-series coordinate system

#### **2. Wrong Data Handling** 
**Problem**: Replacing data instead of accumulating time-series
**Solution**: Each timestamp should create a persistent "slice" on the chart

#### **3. Broken Trade Positioning**
**Problem**: Trades showing as "tetris blocks" instead of time-series dots
**Current logs**: `First point coords: "X:814.477 Y:608.209"` (same X for all trades)
**Solution**: Fix `worldToScreen()` to map timestamps to different X coordinates

#### **4. Order Book Time Slicing**
**Problem**: Order book bars not positioned as time slices
**Solution**: Each order book update needs timestamp-based X positioning

### **🔧 IMMEDIATE FIXES NEEDED**

#### **Fix 1: Clean Coordinate System**
```cpp
// In gpuchartwidget.cpp - Remove all test mode logic
// Keep only: worldToScreen(timestamp, price) → (x, y)
// X-axis: Map timestamp range to 0 → width()
// Y-axis: Map price range to height() → 0
```

#### **Fix 2: Fix Trade Side Comparison**
**Linter Error**: `Invalid operands to binary expression ('const AggressorSide' and 'const char[4]')`
```cpp
// Current broken code:
if (trade.side == "buy") // ❌ Wrong type

// Fix to:
if (trade.side == AggressorSide::Buy) // ✅ Correct enum
```

#### **Fix 3: Remove Test Mode Variables**
**Linter Errors**: `m_testMode` undeclared, `generateTestData` not declared
- Remove `generateTestData()` function completely
- Remove all test mode variables and logic

#### **Fix 4: Time-Series Accumulation**
```cpp
// Current: Replaces 50 points in buffer
// Needed: Accumulates points over time with timestamp-based X positioning

// Algorithm:
// 1. Get earliest and latest timestamps in data
// 2. Map timestamp range to X-axis: [earliest] → [0], [latest] → [width]
// 3. Each trade gets unique X position based on its timestamp
// 4. Order book bars get X position based on snapshot timestamp
```

### **📊 DATA FLOW ARCHITECTURE**

#### **Current Broken Flow**:
```
Trade → convertTradeToGPUPoint → Fixed X position → VBO
OrderBook → Heatmap bars → Fixed screen percentages
```

#### **Target Time-Series Flow**:
```
Trade → (timestamp,price) → worldToScreen(timestamp,price) → (x,y) → VBO
OrderBook → (timestamp,levels) → worldToScreen per level → bars at X=timestamp
```

### **🗂 KEY FILES TO MODIFY**

1. **`libs/gui/gpuchartwidget.cpp`**
   - Fix `worldToScreen()` coordinate mapping
   - Fix `convertTradeToGPUPoint()` to use proper time-series X positioning
   - Remove all test mode code
   - Fix type mismatches (AggressorSide enum)

2. **`libs/gui/heatmapinstanced.cpp`** 
   - Modify order book positioning to use timestamp-based X coordinates
   - Change from fixed screen percentages to time-series slices

3. **`libs/gui/gpuchartwidget.h`**
   - Remove test mode member variables
   - Clean up unused function declarations

### **🎮 EXPECTED USER EXPERIENCE**

#### **Before Subscribe (Clean State)**:
- Empty chart with time/price axes
- No sine wave test data

#### **After Subscribe (Real Data)**:
- Trades appear as colored dots at (timestamp, price) coordinates
- Order book levels appear as vertical bars at specific timestamps  
- Chart builds left-to-right over time
- Pan/zoom works across entire time-series

### **⚡ PERFORMANCE REQUIREMENTS**
- Sub-millisecond coordinate transformations
- Smooth pan/zoom across accumulated data
- Efficient VBO updates for real-time data

### **🔍 DEBUG VERIFICATION**
When working, logs should show:
```
🎯 TIME-SERIES Trade: Price: 107120 Time: 1750954840480 → Screen(245, 387)
🎯 TIME-SERIES Trade: Price: 107128 Time: 1750954840481 → Screen(246, 390)
                                                                     ^^^
                                                           Different X coordinates!
```

### **💡 IMPLEMENTATION PRIORITY**
1. **First**: Fix linter errors and get code compiling
2. **Second**: Implement proper `worldToScreen()` time-series mapping  
3. **Third**: Test with real BTC data to verify accumulation
4. **Fourth**: Integrate order book time slicing

---
**✅ HANDOFF COMPLETE**: Next AI should focus on coordinate system reconstruction with time-series accumulation as the core architecture. 

## 🎯 BREAKTHROUGH UPDATE - VISUALS WORKING! 

**✅ SUCCESS**: Visual elements are now rendering on screen!
- **Order book asks visible** in top-right area
- **Trade data flowing** and coordinate system functional  
- **Real-time data integration** working properly

**📍 REMAINING ISSUE**: Coordinates still positioned too far right
```
🎯 TIME-SERIES Trade: X: 1442.19 (should be ~50-200 for visibility)
🟢 UNIFIED BID BAR: ColumnX: 1676.3 (should align with trades)
```

**🔧 ROOT CAUSE**: Timestamp reference reset logic needs adjustment in `calculateColumnBasedPosition()`

---

## 🎯 TARGET VISUALIZATION

Based on user requirements, we need a **BookMap/Sierra Chart style** time-series display:

```
Time progression →
Price↑  |  T1   T2   T3   T4   T5
107450  | ASK  ASK  ASK  ASK  ASK
107440  |  B         S    B   
107430  |      SS        BB   S
107420  | BID  BID  BID  BID  BID  
```

**Core Requirements:**
- **100ms time slices** = columns (like TradingView candles)
- **Trades positioned** at (timeColumn, priceLevel) 
- **Order book bars** extend left (asks) and right (bids) from time columns
- **Accumulative display** - new data adds columns, doesn't replace
- **User can pan** manually or use auto-scroll to follow latest data

## 💥 CORE ARCHITECTURE PROBLEM

**TWO CONFLICTING COORDINATE SYSTEMS** causing chaos:

### 1. **GPUChartWidget (Trades)** 
- **Column-based time mapping**: `x = (timestampOffset/100ms) * 2px + 50px`
- **Pan/zoom transforms** applied on top
- **Still computing X coordinates too far right** (1400+ range)

### 2. **HeatmapInstanced (Order Book)**
- **Uses same unified coordinate function** but separate rendering
- **Should align with trade columns** but currently offset

## 🔧 IMMEDIATE FIXES NEEDED

### **Priority 1: Coordinate Positioning**
The `calculateColumnBasedPosition()` function in `gpuchartwidget.cpp` needs timestamp offset adjustment:

```cpp
// CURRENT PROBLEM: 
timeOffsetMs = timestamp - s_globalReferenceTimestamp; // Still too large
x = (columnIndex * COLUMN_WIDTH_PX) + 50.0; // Results in X > 1400

// NEEDED FIX:
// Ensure reference timestamp properly resets for real data
// OR implement a sliding window approach starting from left edge
```

### **Priority 2: Pan/Zoom Isolation**
Pan/zoom transforms are affecting coordinate calculation. Consider:
- **Separate coordinate mapping** from view transforms
- **Apply pan/zoom only during rendering**, not during coordinate calculation

## 🛠️ ALTERNATIVE APPROACH SUGGESTION

User suggested a **"start simple"** approach which may be more robust:

1. **Single dot test**: Draw one trade at fixed position (50, 400)
2. **Add pan/zoom**: Verify transforms work on single dot  
3. **Add second dot**: Position to the right based on time offset
4. **Scale up**: Add more dots with proper column spacing
5. **Add order book**: Once dot positioning is solid

This avoids the current Qt/QML coordinate complexity and builds incrementally.

## 📊 CURRENT STATE

### **Working Components:**
- ✅ **Real-time data flow** (trades and order book streaming)
- ✅ **VBO rendering** (50-point batches updating)
- ✅ **Dynamic price ranging** 
- ✅ **Thread-safe data handling**
- ✅ **Visual elements rendering** on screen

### **Broken Components:**
- ❌ **Coordinate positioning** (elements too far right)
- ❌ **Trade/order book alignment** 
- ❌ **Pan/zoom behavior** (affects trades oddly, doesn't affect order book)
- ❌ **Column-based time slicing** (conceptually right, implementation off)

## 🎯 IMPLEMENTATION PLAN

### **Option A: Fix Current System**
1. **Debug timestamp calculation** in `calculateColumnBasedPosition()`
2. **Add better coordinate logging** to trace X positioning
3. **Implement proper reference reset** when switching to real data
4. **Align order book bars** with trade columns

### **Option B: Rebuild with Simpler Approach**  
1. **Create minimal dot renderer** (single trade at fixed position)
2. **Add incremental positioning** (time-based X offsets)
3. **Build up complexity** only after basics work
4. **Migrate current data flow** to new renderer

## 📝 LOG EVIDENCE

**Coordinate Output:**
```
🎯 TIME-SERIES Trade: Price: 107339 Time: 1750958648865 → Screen( 1442.19 , 381.597 )
🟢 UNIFIED BID BAR # 250 : Price 107326 → ColumnX: 1676.3 Y: 124.717
🔴 UNIFIED ASK BAR # 100 : Price 107341 → ColumnX: 1682.3 Y: 45.8724
```

**Widget Dimensions:** `1360 x 774`
**Problem:** X coordinates (1442, 1676, 1682) exceed widget width → elements off-screen

## 🚀 SUCCESS METRICS

When fixed, we should see:
- **Trades positioned** in X range 50-400 (left side of screen)
- **Order book bars** extending left/right from same X coordinates as trades  
- **New columns appearing** as time progresses (every 100ms)
- **Smooth pan/zoom** affecting both trades and order book equally

---

**BREAKTHROUGH**: We have visual rendering! The coordinate calculation is the last piece to fix before this becomes a functional trading terminal. 🎯 