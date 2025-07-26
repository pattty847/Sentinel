# 🔥 PHASE 2 HANDOFF: Heatmap Order Book Investigation
## Current Status & Critical Issues to Resolve

---

## 🎯 **WHAT WE'VE ACCOMPLISHED**

### ✅ **Phase 2 Core Implementation Complete**
- **HeatmapInstanced component**: `libs/gui/heatmapinstanced.h/cpp` 
- **Separate bid/ask buffers**: Green quads (bids), Red quads (asks)
- **GPU rendering working**: Two-draw-call architecture with `QSGFlatColorMaterial`
- **Critical shader fix applied**: Fixed attribute/shader mismatch (24-byte → 8-byte vertices)
- **QML integration**: Added to `libs/gui/qml/DepthChartView.qml`
- **CMake integration**: Added to `libs/gui/CMakeLists.txt`

### ✅ **Visual Progress**
- **No more massive triangular fills**: GPT o3's vertex attribute fix resolved the "green monster"
- **Small discrete quads rendering**: We see individual red squares instead of screen-filling wedges
- **Both layers active**: Phase 1 (trade points) + Phase 2 (heatmap) simultaneously

---

## 🚨 **CRITICAL ISSUE: HEATMAP NOT SHOWING ORDER BOOK LEVELS**

### **What User Sees:**
- Scattered red squares that **repaint and disappear** when new data arrives
- **NOT** organized horizontal bars at specific price levels
- Colors change (red/green/yellow) suggesting both Phase 1 and Phase 2 are rendering
- Looks like "trade points repainting" rather than "order book depth visualization"

### **Missing Debug Output:**
Expected but **NOT SEEN** in logs:
```cpp
🟢 BID BAR: Price 107283 → Y: 350 Width: 25
🔴 ASK BAR: Price 107290 → Y: 340 Width: 30
```

### **Data Flow Disconnect:**
```
✅ Trade Data: WebSocket → MarketDataCore → StreamController → GPUChartWidget (WORKING)
❌ Order Book: WebSocket → MarketDataCore → ??? → HeatmapInstanced (BROKEN)
```

---

## 🔍 **CRITICAL FINDING: ORDER BOOK SIGNAL EXISTS!**

### **StreamController has the signal we need:**
```cpp
// FROM libs/gui/StreamController.h - LINE 39:
void orderBookUpdated(const OrderBook& book);  ✅ SIGNAL EXISTS!

// AND polling method exists:
private slots:
    void pollForOrderBooks();  ✅ POLLING METHOD EXISTS!
```

### **This narrows the issue to:**
1. **Is `pollForOrderBooks()` being called?** Timer setup issue?
2. **Is signal actually emitted?** Check StreamController.cpp implementation
3. **Is HeatmapInstanced connected?** QML connection missing?
4. **Is order book data empty/wrong format?** Data structure mismatch?

---

## 🔍 **INVESTIGATION ROADMAP**

### **1. Trace Order Book Data Flow**

**Files to Examine:**
- `libs/core/MarketDataCore.cpp/hpp` - Where order book comes from WebSocket
- `libs/gui/streamcontroller.cpp/hpp` - Should bridge order book to GUI
- `libs/gui/heatmapinstanced.cpp` - Where order book should be consumed

**Key Questions:**
1. **Is order book data being received?** Look for logs: `📊 BTC-USD order book: X bids, Y asks`
2. **Is StreamController emitting order book signals?** Should see: `emit orderBookUpdated(book)`
3. **Is HeatmapInstanced connected to signals?** Check QML connections
4. **Is order book data in right price range?** Real data ~$107,283 vs test range 107100-107250

### **2. Current Data Connections**

**Working Connection (Phase 1):**
```cpp
// In QML:
connect(streamController, &StreamController::tradeReceived,
        gpuChart, &GPUChartWidget::onTradeReceived);
```

**Missing Connection (Phase 2):**
```cpp
// SIGNAL EXISTS IN STREAMCONTROLLER.H! ✅
void orderBookUpdated(const OrderBook& book);

// CONNECTION NEEDED in QML:
connect(streamController, &StreamController::orderBookUpdated,
        heatmapLayer, &HeatMapInstanced::onOrderBookUpdated);
```

### **3. Debugging Strategy**

**Step 1: Check StreamController Implementation**
```bash
# Check if pollForOrderBooks() actually emits the signal:
grep -A 10 -B 5 "pollForOrderBooks" libs/gui/streamcontroller.cpp
grep -A 5 -B 5 "orderBookUpdated" libs/gui/streamcontroller.cpp
```

**Step 2: Verify Timer Setup**
```bash
# Check if order book polling timer is started:
grep -A 5 -B 5 "m_orderBookPollTimer" libs/gui/streamcontroller.cpp
```

**Step 3: Test Heatmap Connection**
- Create QML button to call `heatmapLayer.generateTestOrderBook()`
- Verify test quads appear as horizontal bars, not scattered dots

---

## 📁 **KEY FILES TO EXAMINE**

### **Order Book Data Source:**
- `libs/core/MarketDataCore.cpp` - Lines containing `order book`
- `libs/core/TradeData.h` - `OrderBook` and `OrderBookLevel` structs

### **Data Bridge (HIGH PRIORITY):**
- `libs/gui/streamcontroller.cpp` - Implementation of `pollForOrderBooks()`
- `libs/gui/StreamController.h` - Signal exists: `void orderBookUpdated(const OrderBook& book);`

### **Heatmap Consumer:**
- `libs/gui/heatmapinstanced.cpp` - `onOrderBookUpdated()` method
- `libs/gui/qml/DepthChartView.qml` - Signal connections

### **Integration Point:**
- `libs/gui/mainwindow_gpu.cpp` - Where components are wired together

---

## 🧠 **TECHNICAL CONTEXT FOR NEXT SESSION**

### **Architecture:**
```cpp
WebSocket (Coinbase) 
    ↓ 
MarketDataCore::handleOrderBookMessage()
    ↓
StreamController::pollForOrderBooks() ← INVESTIGATE THIS!
    ↓
emit orderBookUpdated(OrderBook)  ← SIGNAL EXISTS!
    ↓
HeatmapInstanced::onOrderBookUpdated()
    ↓
convertOrderBookToInstances() → GPU rendering
```

### **Data Structures:**
```cpp
struct OrderBookLevel {
    double price;   // e.g., 107283.50
    double size;    // e.g., 2.5 BTC
};

struct OrderBook {
    std::vector<OrderBookLevel> bids;  // Buy orders (green, right side)
    std::vector<OrderBookLevel> asks;  // Sell orders (red, left side)
};
```

### **Expected Visual Result:**
```
Price    Asks (Red)     |     Bids (Green)
107290   ████           |           ██████
107285   ██             |         ████████  
107280   ██████         |           ████
107275                  |         ██████
```

---

## 🎯 **IMMEDIATE NEXT STEPS**

### **Priority 1: Check StreamController Implementation**
```bash
# FIRST: Check if pollForOrderBooks() actually works:
grep -A 15 "void StreamController::pollForOrderBooks" libs/gui/streamcontroller.cpp

# SECOND: Check if timer is started:
grep -A 10 -B 10 "m_orderBookPollTimer" libs/gui/streamcontroller.cpp
```

### **Priority 2: Debug Real vs Test Data**
1. **Verify price ranges match:**
   - Test data: 107100-107250
   - Real trades: ~107283
   - Fix range in `DepthChartView.qml`

2. **Add debug logging to heatmap:**
   ```cpp
   qDebug() << "📊 HEATMAP: Received" << book.bids.size() 
            << "bids," << book.asks.size() << "asks";
   ```

### **Priority 3: Quick Win Test**
Create manual trigger in QML:
```qml
Button {
    text: "Test Heatmap"
    onClicked: heatmapLayer.generateTestOrderBook()
}
```

---

## 📋 **SUCCESS CRITERIA**

**Phase 2 Complete When:**
1. ✅ **Horizontal bars visible** at specific price levels
2. ✅ **Bids on right (green)**, asks on left (red)
3. ✅ **Bar width** proportional to order size
4. ✅ **Real-time updates** as order book changes
5. ✅ **No repainting glitches** or scattered dots

**Performance Target:** <2ms GPU time @ 200k quads

---

## 🚀 **PHONE A FRIEND CONTEXT**

If stuck, provide this context to another AI:

**Problem:** "Phase 2 heatmap shows scattered red squares instead of organized horizontal price-level bars. Order book data (~107283 BTC) may not be reaching HeatmapInstanced component. Trade data works fine (Phase 1), but order book visualization is disconnected."

**Architecture:** Qt Quick + QSGGeometry + real-time WebSocket data from Coinbase

**Key Question:** "StreamController has `orderBookUpdated` signal and `pollForOrderBooks()` method, but heatmap isn't showing order book levels. Is the signal being emitted? Is QML connection missing?"

---

**🔥 Ready for next Claude session to hunt down the missing order book connection!** 