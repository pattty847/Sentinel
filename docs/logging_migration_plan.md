# Sentinel Logging Migration Plan

## Problem Analysis

From the 3-second run logs, we identified these major issues:

### High-Frequency Spam (100+ messages/second)
- **🕯️ CANDLE RENDER UPDATE** - Happens every paint cycle (~60Hz)
- **✅ SCENE GRAPH VALIDATION** - Qt scene graph operations
- **🎨 PAINT NODE UPDATE** - Geometry updates
- **🗺️ UNIFIED COORD CALC** - Coordinate calculations

### Medium-Frequency Verbose Logs (10-50/second)
- **💰 BTC-USD trade logs** - Every trade with detailed info
- **📤 GPU queue operations** - Data pipeline logs
- **🎯 Trade color processing** - Color/size calculations
- **🔍 Cache access patterns** - Order book lookups

### Initialization Spam (useful once, not repeatedly)
- **🚀 Component initialization** - Should be one-time only
- **🔥 Connection establishment** - Network handshake logs
- **✅ Subscription confirmations** - Market data subscriptions

## New Qt Logging Categories

### By Component
```cpp
// CORE
Q_LOGGING_CATEGORY(logCore, "sentinel.core")             // Auth, data structures
Q_LOGGING_CATEGORY(logNetwork, "sentinel.network")       // WebSocket, connections
Q_LOGGING_CATEGORY(logCache, "sentinel.cache")           // DataCache, trades, books

// GUI
Q_LOGGING_CATEGORY(logRender, "sentinel.render")         // Paint operations
Q_LOGGING_CATEGORY(logRenderDetail, "sentinel.render.detail") // Geometry details
Q_LOGGING_CATEGORY(logChart, "sentinel.chart")           // Chart operations
Q_LOGGING_CATEGORY(logCandles, "sentinel.candles")       // Candlestick processing
Q_LOGGING_CATEGORY(logTrades, "sentinel.trades")         // Trade visualization

// LIFECYCLE
Q_LOGGING_CATEGORY(logInit, "sentinel.init")             // One-time initialization
Q_LOGGING_CATEGORY(logConnection, "sentinel.connection") // Connection lifecycle
```

### By Frequency/Severity
```cpp
// DEBUG (disabled by default)
Q_LOGGING_CATEGORY(logDebugCoords, "sentinel.debug.coords")     // Coordinate calcs
Q_LOGGING_CATEGORY(logDebugGeometry, "sentinel.debug.geometry") // Geometry details
Q_LOGGING_CATEGORY(logDebugTiming, "sentinel.debug.timing")     // Frame timing
```

## Migration Examples

### Before: Uncontrolled Spam
```cpp
// This runs 100+ times per second!
qDebug() << "🕯️ CANDLE RENDER UPDATE: LOD:" << timeframe << "Total:" << count;
qDebug() << "✅ SCENE GRAPH VALIDATION PASSED: Returning valid node";
```

### After: Controlled Categories
```cpp
// High-frequency rendering - can be disabled
sLog_Render() << "🕯️ CANDLE RENDER UPDATE: LOD:" << timeframe << "Total:" << count;
sLog_RenderDetail() << "✅ SCENE GRAPH VALIDATION PASSED: Returning valid node";

// Or use throttled logging for high-frequency events
gRenderLogger.log(logRender, "🕯️ CANDLE RENDER UPDATE:", timeframe);
```

### Before: Repetitive Initialization
```cpp
// This runs every time a widget is created
qDebug() << "🚀 CREATING GPU TRADING TERMINAL!";
qDebug() << "🔥 HeatmapInstanced created - Phase 2 GPU rendering ready!";
```

### After: Proper Initialization
```cpp
// Only logs during initialization phase
sLog_Init() << "🚀 CREATING GPU TRADING TERMINAL!";
sLog_Init() << "🔥 HeatmapInstanced created - Phase 2 GPU rendering ready!";
```

### Before: Verbose Trade Processing
```cpp
// Logs every single trade with full details
static int count = 0;
if (++count % 20 == 1) {
    qDebug() << "💰 BTC-USD:" << price << "size:" << size << "[" << count << " trades total]";
}
```

### After: Throttled Trade Logging
```cpp
// Clean throttled logging with proper categorization
gTradeLogger.log(logTrades, "💰 BTC-USD:", price, "size:", size);
```

## Environment Variable Control

### Development Mode (see everything)
```bash
export QT_LOGGING_RULES="sentinel.*.debug=true"
```

### Production Mode (errors only)
```bash
export QT_LOGGING_RULES="*.debug=false;*.warning=true;*.critical=true"
```

### Component-Specific Debugging
```bash
# Only network and cache logs
export QT_LOGGING_RULES="sentinel.network.debug=true;sentinel.cache.debug=true"

# Disable high-frequency rendering spam
export QT_LOGGING_RULES="sentinel.render.detail.debug=false;sentinel.debug.*.debug=false"

# Trading focus (trades and charts only)
export QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.chart.debug=true;sentinel.candles.debug=true"
```

### Performance Testing Mode
```bash
# Only critical errors and performance metrics
export QT_LOGGING_RULES="*.debug=false;sentinel.performance.debug=true;*.critical=true"
```

## Migration Priority

### Phase 1: High-Impact Components (Immediate Spam Reduction)
1. **CandlestickBatched.cpp** - Biggest offender with paint cycle spam
2. **GPUChartWidget.cpp** - Coordinate calculation spam  
3. **HeatmapInstanced.cpp** - Scene graph validation spam
4. **GPUDataAdapter.cpp** - Data pipeline spam

### Phase 2: Medium-Frequency Components
1. **StreamController.cpp** - Trade processing logs
2. **MarketDataCore.cpp** - Network operation logs
3. **DataCache.cpp** - Cache access logs

### Phase 3: Initialization Components  
1. **MainWindow.cpp** - Application startup
2. **Component constructors** - One-time initialization

## Expected Results

### Before Migration (3-second run)
- **580+ log lines** in 3 seconds
- **Constant rendering spam** every 16ms (60Hz)
- **Repetitive initialization** logs
- **Impossible to debug** specific issues

### After Migration (Production Mode)
- **<50 log lines** in 3 seconds
- **Only errors and warnings** by default
- **Selective debugging** by component
- **Readable, actionable** logs

### After Migration (Development Mode)
- **Categorized logs** by component
- **Throttled high-frequency** operations  
- **Environment control** over verbosity
- **Clear debugging** workflow

## Implementation Steps

1. ✅ **Create logging system** - `SentinelLogging.hpp/cpp`
2. ✅ **Add to build system** - Update CMakeLists.txt
3. 🔄 **Migrate high-impact files** - CandlestickBatched first
4. ⏳ **Test environment controls** - Verify filtering works
5. ⏳ **Migrate remaining components** - Systematic rollout
6. ⏳ **Create debugging guides** - Document best practices

This migration will transform the logging chaos into a professional, controllable system that actually helps with debugging instead of drowning us in noise. 