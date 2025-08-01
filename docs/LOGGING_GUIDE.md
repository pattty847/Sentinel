# Sentinel Logging Usage Guide

## Quick Start

### 🔥 Immediate Relief from Render Log Spam

**Problem**: Your logs are flooded with render messages like "OPTIMIZED RENDER", "UNIFIED GRID RENDER COMPLETE", "CACHE CHECK"

**Solution**: The render logging has been throttled to reduce spam:
- Render completion: Now logs every 50th frame instead of every frame
- Optimized render: Now logs every 10th rebuild instead of every rebuild  
- Transform updates: Now logs every 25th transform instead of every transform
- Cache checks: Reduced frequency from every 50th to every 100th check
- Trade updates: Now logs every 20th trade instead of every trade
- Geometry changes: Now logs every 5th change instead of every change

**For even cleaner logs**, set this environment variable:
```bash
export QT_LOGGING_RULES="sentinel.render.detail.debug=false;sentinel.debug.*.debug=false"
```

This disables the high-frequency rendering spam while keeping useful logs.

### 🚀 Render Log Throttling Implementation

The render system now uses intelligent throttling to reduce log spam while preserving useful information:

**Throttled Messages:**
- `🎯 UNIFIED GRID RENDER COMPLETE`: Every 50th frame (was every frame)
- `🎯 OPTIMIZED RENDER`: Every 10th rebuild (was every rebuild)  
- `🎯 TRANSFORM-ONLY UPDATE`: Every 25th transform (was every transform)
- `🔍 CACHE CHECK`: Every 100th check (was every 50th)
- `📊 TRADE UPDATE`: Every 20th trade (was every trade)
- `🎯 UNIFIED RENDERER GEOMETRY CHANGED`: Every 5th change (was every change)

**Implementation Details:**
- Uses static counters with modulo operations
- Preserves frame/rebuild counts in log messages for debugging
- Maintains all functionality while reducing noise
- Can be adjusted by modifying the modulo values in `UnifiedGridRenderer.cpp`

## Environment Variable Control

### Production Mode (Clean)
```bash
export QT_LOGGING_RULES="*.debug=false;*.warning=true;*.critical=true"
./build/apps/sentinel_gui/sentinel
```
**Result**: Only errors and warnings (~5-10 lines total)

### Development Mode (Everything)
```bash
export QT_LOGGING_RULES="sentinel.*.debug=true"
./build/apps/sentinel_gui/sentinel
```
**Result**: All logs enabled (~200+ lines)

### Focused Debugging

#### Trading Issues
```bash
export QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.cache.debug=true;sentinel.network.debug=true"
```
**Shows**: Trade processing, data caching, network operations

#### Rendering Issues  
```bash
export QT_LOGGING_RULES="sentinel.chart.debug=true;sentinel.candles.debug=true;sentinel.render.debug=true"
```
**Shows**: Chart operations, candlestick processing, basic rendering

#### Performance Issues
```bash
export QT_LOGGING_RULES="sentinel.performance.debug=true;sentinel.debug.timing.debug=true"
```
**Shows**: Performance metrics and detailed timing

#### Coordinate/Layout Issues
```bash
export QT_LOGGING_RULES="sentinel.debug.coords.debug=true;sentinel.debug.geometry.debug=true"
```
**Shows**: Detailed coordinate calculations and geometry creation

### Network/Connection Issues
```bash
export QT_LOGGING_RULES="sentinel.network.debug=true;sentinel.connection.debug=true;sentinel.subscription.debug=true"
```
**Shows**: WebSocket connections, subscriptions, network handshakes

## Log Categories Reference

### Core Components
| Category | Purpose | Frequency | Default |
|----------|---------|-----------|---------|
| `sentinel.core` | Auth, data structures | Low | ✅ On |
| `sentinel.network` | WebSocket, connections | Low | ✅ On |
| `sentinel.cache` | DataCache operations | Medium | ✅ On |
| `sentinel.performance` | Performance metrics | Low | ✅ On |

### GUI Components
| Category | Purpose | Frequency | Default |
|----------|---------|-----------|---------|
| `sentinel.render` | Basic rendering | High | ✅ On |
| `sentinel.render.detail` | Paint/geometry details | Very High | ❌ Off |
| `sentinel.chart` | Chart operations | Medium | ✅ On |
| `sentinel.candles` | Candlestick processing | Medium | ✅ On |
| `sentinel.trades` | Trade visualization | High | ✅ On |
| `sentinel.camera` | Pan/zoom operations | High | ✅ On |
| `sentinel.gpu` | GPU data pipeline | High | ✅ On |

### Lifecycle
| Category | Purpose | Frequency | Default |
|----------|---------|-----------|---------|
| `sentinel.init` | Component initialization | One-time | ✅ On |
| `sentinel.connection` | Connection lifecycle | Low | ✅ On |
| `sentinel.subscription` | Market subscriptions | Low | ✅ On |

### Debug Categories
| Category | Purpose | Frequency | Default |
|----------|---------|-----------|---------|
| `sentinel.debug.coords` | Coordinate calculations | Very High | ❌ Off |
| `sentinel.debug.geometry` | Geometry creation | Very High | ❌ Off |
| `sentinel.debug.timing` | Frame timing | Very High | ❌ Off |
| `sentinel.debug.data` | Raw data processing | Very High | ❌ Off |

## Common Debugging Scenarios

### "My app is too slow"
```bash
export QT_LOGGING_RULES="sentinel.performance.debug=true;sentinel.debug.timing.debug=true"
```
Look for performance metrics and frame timing issues.

### "Trades aren't showing up"
```bash
export QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.network.debug=true;sentinel.cache.debug=true"
```
Track trade data from network → cache → visualization.

### "Charts are rendering incorrectly"
```bash
export QT_LOGGING_RULES="sentinel.chart.debug=true;sentinel.candles.debug=true;sentinel.debug.coords.debug=true"
```
Debug coordinate calculations and chart rendering logic.

### "Can't connect to Coinbase"
```bash
export QT_LOGGING_RULES="sentinel.network.debug=true;sentinel.connection.debug=true;sentinel.subscription.debug=true"
```
Debug WebSocket handshake and subscription process.

### "App crashes during rendering"
```bash
export QT_LOGGING_RULES="sentinel.render.detail.debug=true;sentinel.debug.geometry.debug=true"
```
Enable detailed rendering logs to catch geometry issues.

## Advanced Usage

### Multiple Rules
```bash
export QT_LOGGING_RULES="sentinel.trades.debug=true;sentinel.network.debug=false;*.warning=true"
```

### Wildcards
```bash
# Enable all debug categories
export QT_LOGGING_RULES="sentinel.debug.*.debug=true"

# Disable all rendering
export QT_LOGGING_RULES="sentinel.render*.debug=false"
```

### Runtime Control
```bash
# Set via QML/C++ code
QLoggingCategory::setFilterRules("sentinel.trades.debug=true");
```

## Migration Status

### ✅ Ready Components
- Core logging system implemented
- All categories defined
- Build system integrated
- Environment control working

### 🔄 In Progress  
- Migrating CandlestickBatched.cpp
- Testing environment controls
- Documenting best practices

### ⏳ Pending Components
- GPUChartWidget.cpp
- HeatmapInstanced.cpp
- StreamController.cpp
- MarketDataCore.cpp
- All remaining GUI components

## Before/After Comparison

### Before Migration
```
🚀 [Sentinel GPU Trading Terminal Starting...]
🚀 CREATING GPU TRADING TERMINAL!
🔍 Trying QML path: "/path/to/qml"
🔥 HeatmapInstanced created - Phase 2 GPU rendering ready!
🕯️ CandlestickBatched INITIALIZED - Professional Trading Terminal Candles!
🎯 LOD System: Enabled | Max Candles: 10000 | Auto-scaling: ON
🚀 GPUChartWidget OPTION B REBUILD - CLEAN COORDINATE SYSTEM!
💾 Max Points: 100000 | Time Span: 60000 ms
🎯 Single coordinate system - no test mode, no sine waves
... (570+ more lines)
```

### After Migration (Production Mode)
```
⚠️ GPU Trading Terminal Starting...
✅ WebSocket connected to Coinbase
✅ Subscribed to BTC-USD market data
✅ GUI initialized successfully
(End of logs - clean and professional)
```

### After Migration (Debug Mode)
```bash
export QT_LOGGING_RULES="sentinel.trades.debug=true"
```
```
🚀 GPU Trading Terminal Starting...
✅ WebSocket connected to Coinbase
✅ Subscribed to BTC-USD market data
💰 BTC-USD: $108259 size:0.00088999 (BUY) [call #1]
💰 BTC-USD: $108260 size:0.00045724 (SELL) [call #21]
💰 BTC-USD: $108259 size:0.00069738 (BUY) [call #41]
(Only trade logs - perfect for debugging trade issues)
```

## Best Practices

1. **Start with production mode** to see only errors
2. **Add specific categories** as needed for debugging
3. **Use throttled loggers** for high-frequency events
4. **Disable debug categories** in production builds
5. **Document your logging rules** in development scripts

This system transforms the logging chaos into a powerful debugging tool! 