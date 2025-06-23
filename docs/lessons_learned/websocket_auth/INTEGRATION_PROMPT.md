# 🚀 INTEGRATION TASK: Merge Working WebSocket Auth with Existing GUI Architecture

## 🎯 MISSION
We have **WORKING** Coinbase WebSocket authentication in `simple_coinbase.cpp` that successfully connects and receives data. Now we need to integrate this authentication method into the existing `CoinbaseStreamClient` class and get real-time data flowing to the GUI.

## ✅ WHAT'S ALREADY WORKING
1. **Perfect WebSocket Auth**: `simple_coinbase.cpp` successfully:
   - Generates ES256 JWT tokens using `jwt-cpp` library
   - Connects to `advanced-trade-ws.coinbase.com`
   - Subscribes to `level2` channel with JWT authentication
   - Receives real-time order book data

2. **Complete GUI Architecture**: 
   - `CoinbaseStreamClient` class with proper Boost.Beast async architecture
   - `StreamController` bridges core and GUI with Qt signals/slots
   - `TradeChartWidget` ready to display data
   - Proper data structures in `tradedata.h`

## 🔧 INTEGRATION TASKS

### Task 1: Fix CoinbaseStreamClient Authentication
**File**: `libs/core/coinbasestreamclient.cpp`

**Problem**: Current implementation has broken authentication logic
**Solution**: Copy the WORKING JWT generation from `simple_coinbase.cpp`

**Key Changes Needed**:
1. Fix `generateJwt()` method - copy from `simple_coinbase.cpp` lines 23-42
2. Fix `buildSubscribeMessage()` - use JWT in message body, not headers
3. Update subscription format to match working version

### Task 2: Update Data Processing Pipeline
**Files**: `libs/core/coinbasestreamclient.cpp`, `libs/gui/streamcontroller.cpp`

**Current Issue**: Data flows to console but not to GUI
**Solution**: 
1. Parse incoming JSON in `CoinbaseStreamClient::on_read()`
2. Convert to `Trade` and `OrderBook` structs
3. Emit signals to `StreamController`
4. Forward to GUI widgets

### Task 3: Connect StreamController to GUI
**Files**: `libs/gui/streamcontroller.cpp`, `apps/sentinel_gui/main.cpp`

**Need To**:
1. Connect `StreamController` signals to `TradeChartWidget` slots
2. Ensure proper data flow: WebSocket → StreamClient → StreamController → GUI
3. Test with real-time data display

## 📋 SPECIFIC INTEGRATION STEPS

### Step 1: Copy Working JWT Logic
```cpp
// FROM simple_coinbase.cpp lines 23-42
// TO libs/core/coinbasestreamclient.cpp generateJwt() method
```

### Step 2: Fix Subscribe Message Format
```cpp
// Current (BROKEN):
// Headers with CB-ACCESS-* 

// Working (FROM simple_coinbase.cpp):
nlohmann::json subscribe_msg;
subscribe_msg["type"] = "subscribe";
subscribe_msg["product_ids"] = {"BTC-USD"};
subscribe_msg["channel"] = "level2";
subscribe_msg["jwt"] = jwt_token;
```

### Step 3: Parse and Forward Data
```cpp
// In CoinbaseStreamClient::on_read():
// 1. Parse JSON response
// 2. If level2 data → create OrderBook struct
// 3. If matches data → create Trade struct  
// 4. Emit signals to StreamController
```

## 🏗️ ARCHITECTURE FLOW
```
simple_coinbase.cpp (WORKING) 
    ↓ (copy JWT logic)
CoinbaseStreamClient 
    ↓ (emit signals)
StreamController 
    ↓ (Qt signals/slots)
TradeChartWidget (GUI)
```

## 🔑 CRITICAL SUCCESS FACTORS

1. **DON'T BREAK THE WORKING AUTH**: Copy JWT logic exactly from `simple_coinbase.cpp`
2. **USE EXISTING ARCHITECTURE**: Don't rewrite, just integrate
3. **FOLLOW DATA FLOW**: WebSocket → Parse → Emit → Display
4. **TEST INCREMENTALLY**: Verify each step works before moving to next

## 📁 KEY FILES TO MODIFY
- `libs/core/coinbasestreamclient.cpp` - Fix JWT auth & data parsing
- `libs/gui/streamcontroller.cpp` - Connect signals to GUI
- `apps/sentinel_gui/main.cpp` - Wire up the connections

## 🚨 BUILD REQUIREMENTS
- `jwt-cpp` library already added to `vcpkg.json`
- All dependencies should be working from previous session
- Use existing CMake build system

## 💡 DEBUGGING TIPS
1. Start with console output to verify data flow
2. Add debug prints at each step of the pipeline
3. Test WebSocket connection first, then GUI integration
4. Use the working `simple_coinbase.cpp` as reference

## 🎯 SUCCESS CRITERIA
- [ ] CoinbaseStreamClient connects with JWT auth
- [ ] Real-time level2 data flows to console
- [ ] Data parsed into Trade/OrderBook structs
- [ ] GUI displays live order book updates
- [ ] No crashes or connection failures

---

**REMEMBER**: We have working authentication in `simple_coinbase.cpp` - just need to integrate it properly with the existing architecture. Don't overthink it, just copy what works! 