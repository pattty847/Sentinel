<!-- 11a0e186-875a-42fa-9597-c4404ced7250 8605bc6c-8eda-4b09-b83a-44d60e58aea8 -->
# Tick Candlestick Render Strategy

## Overview

Create a new `TickCandleStrategy` that processes raw trades from `GridSliceBatch::recentTrades` to build OHLC candlesticks. The strategy will aggregate N trades per candle (default: 2) or create candles when price changes, rendering proper candlestick geometry with wicks and bodies.

## Architecture Context

- **Data Flow**: `MarketDataCore` → `UnifiedGridRenderer::m_recentTrades` → `GridSliceBatch::recentTrades` → `TickCandleStrategy`
- **Pattern**: Follows `TradeBubbleStrategy` pattern (uses `batch.recentTrades` instead of `batch.cells`)
- **Rendering**: Uses `QSGGeometryNode` with `QSGVertexColorMaterial` for GPU rendering, similar to `CandleStrategy`

## Implementation Steps

### 1. Create TickCandleStrategy class

- **File**: `libs/gui/render/strategies/TickCandleStrategy.hpp` and `.cpp`
- Inherit from `IRenderStrategy`
- Implement `buildNode()` to process `batch.recentTrades`
- Aggregate trades into OHLC candles:
- **Mode 1**: Fixed N-tick aggregation (default N=2)
- **Mode 2**: Price-change-based (create candle when price moves)
- Calculate OHLC from grouped trades:
- Open: first trade price in group
- High: max price in group
- Low: min price in group  
- Close: last trade price in group
- Render candlestick geometry:
- Body: rectangle from open to close (bullish=green, bearish=red)
- Wicks: lines from high/low to body edges
- Use `CoordinateSystem::worldToScreen()` for positioning

### 2. Add configuration methods

- `setTicksPerCandle(int n)` - set N for fixed aggregation (default: 2)
- `setPriceChangeMode(bool enabled)` - enable price-change-based mode
- `setWickThickness(float)` - control wick line width
- `setBodyWidthRatio(float)` - control body width relative to time span

### 3. Integrate into UnifiedGridRenderer

- **File**: `libs/gui/UnifiedGridRenderer.h` and `.cpp`
- Add `RenderMode::TickCandles` enum value
- Create `m_tickCandleStrategy` member (similar to `m_candleStrategy`)
- Initialize in `init()` method
- Wire into `getCurrentStrategy()` switch statement
- Add to `updateLayeredContent()` calls (if needed for layering)

### 4. Update CMakeLists.txt

- **File**: `libs/gui/CMakeLists.txt`
- Add `TickCandleStrategy.cpp` to source files list

## Design Decisions

- **Trade Filtering**: Filter trades by viewport bounds (time and price) before aggregation
- **Candle Positioning**: Use candle's time range (first trade timestamp to last trade timestamp) for X-axis positioning
- **Color Scheme**: Green for bullish (close >= open), red for bearish (close < open)
- **Performance**: Limit processed trades similar to `TradeBubbleStrategy` (respect `batch.maxCells`)

## Files to Modify

1. `libs/gui/render/strategies/TickCandleStrategy.hpp` (new)
2. `libs/gui/render/strategies/TickCandleStrategy.cpp` (new)
3. `libs/gui/UnifiedGridRenderer.h` - add enum and member
4. `libs/gui/UnifiedGridRenderer.cpp` - initialize and wire strategy
5. `libs/gui/CMakeLists.txt` - add source file

## Testing Considerations

- Verify candles render correctly with N=2, N=5, N=10
- Test price-change mode creates candles only when price moves
- Ensure viewport filtering works (only visible candles render)
- Check performance with high trade volumes (1000+ trades)

### To-dos

- [ ] Create TickCandleStrategy.hpp with IRenderStrategy interface, OHLC aggregation logic, and configuration methods
- [ ] Implement TickCandleStrategy.cpp with trade aggregation (N-tick and price-change modes), OHLC calculation, and candlestick geometry rendering
- [ ] Add RenderMode::TickCandles to UnifiedGridRenderer.h enum and create m_tickCandleStrategy member
- [ ] Initialize TickCandleStrategy in UnifiedGridRenderer::init() and wire into getCurrentStrategy() and rendering pipeline
- [ ] Add TickCandleStrategy.cpp to libs/gui/CMakeLists.txt source files