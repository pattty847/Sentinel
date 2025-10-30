# Candlesticks and Trade Bubbles Implementation Plan

## Overview
Implement proper OHLC candlestick rendering and trade bubble visualization to complement the existing heatmap strategy. This will provide multiple ways to visualize market microstructure data.

## Goals
- **Tick-based candlesticks** with proper bodies and wicks
- **Trade bubble visualization** showing individual trade flow
- **Multiple candle modes** (regular, tick-based, Heiken-Ashi)
- **Seamless mode switching** between heatmap, candles, and bubbles

## Current State
- ✅ **CandleStrategy exists** but renders volume rectangles, not OHLC candlesticks
- ✅ **TradeFlowStrategy exists** but renders circles, not proper trade bubbles  
- ✅ **Trade data available** via `Trade` struct with price, size, timestamp, side
- ✅ **CellInstance structure** ready for extension

## Implementation Plan

### Phase 1: Extend Data Structures
**Extend CellInstance (Option 1 approach)**
```cpp
struct CellInstance {
    // Existing heatmap data
    double liquidity = 0.0;
    bool isBid = true; 
    double intensity = 0.0;
    
    // Add OHLC data
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
    int tradeCount = 0;
    
    // Trade bubble data
    std::vector<Trade> trades;  // Individual trades for bubble mode
    
    // Computed properties
    bool hasOHLC() const { return high > 0.0 && low > 0.0; }
    bool isBullish() const { return close >= open; }
    double bodyTop() const { return std::max(open, close); }
    double bodyBottom() const { return std::min(open, close); }
};
```

### Phase 2: Trade Accumulation Logic
**DataProcessor enhancements:**
- **Tick-based grouping**: Accumulate N trades into single candle
- **Time-based grouping**: Alternative grouping by time intervals
- **OHLC calculation**: Extract open/high/low/close from trade sequences
- **Populate both modes**: Fill heatmap data AND OHLC data simultaneously

**Key files to modify:**
- `libs/gui/render/DataProcessor.cpp` - Add trade accumulation logic
- `libs/gui/render/GridTypes.hpp` - Extend CellInstance structure

### Phase 3: Candlestick Rendering
**CandleStrategy overhaul:**
- **Proper OHLC rendering**: Bodies (open→close) + wicks (high/low lines)
- **Color schemes**: Bullish (green) vs bearish (red) candles
- **Multiple modes**: 
  - Regular OHLC candles
  - Tick-based candles (N trades per candle)
  - Heiken-Ashi smoothed candles
- **Performance**: Single QSGNode with efficient triangle rendering

**Rendering components:**
- **Candle body**: Rectangle from open to close price
- **Upper wick**: Thin line from body top to high
- **Lower wick**: Thin line from body bottom to low
- **Width scaling**: Based on time span and zoom level

### Phase 4: Trade Bubble Visualization
**TradeFlowStrategy enhancement:**
- **Bubble sizing**: Size based on trade volume
- **Bubble positioning**: Exact price/time coordinates
- **Color coding**: Buy vs sell, volume intensity
- **Animation**: Optional fade-in/fade-out for live trades
- **Clustering**: Group nearby trades to avoid overlap

**Bubble features:**
- **Size**: Proportional to trade size
- **Color**: Green (buy) / Red (sell) with intensity based on volume
- **Position**: Exact timestamp and price coordinates
- **Tooltip data**: Show trade details on hover

### Phase 5: Mode Switching UI
**Visualization mode controls:**
- **Heatmap mode**: Current liquidity visualization
- **Candlestick mode**: OHLC candles with configurable tick grouping
- **Trade bubble mode**: Individual trade visualization
- **Hybrid modes**: Candles + bubbles, heatmap + bubbles

**Configuration options:**
- **Tick size**: 10, 50, 100, 500, 1000 trades per candle
- **Candle type**: Regular, Heiken-Ashi, Renko
- **Bubble size scaling**: Linear, logarithmic, fixed
- **Color schemes**: Multiple palettes for different modes

## Technical Considerations

### Performance
- **Single QSGNode per strategy**: Maintain current efficient rendering
- **Vertex buffer management**: Pre-allocate based on visible cells
- **LOD scaling**: Reduce detail at high zoom levels
- **Culling**: Skip rendering for off-screen elements

### Data Flow
- **Unified pipeline**: Same DataProcessor feeds all visualization modes
- **Memory efficiency**: Reuse CellInstance for multiple strategies
- **Thread safety**: Maintain current Qt render thread architecture
- **Real-time updates**: Efficient incremental updates for live data

### Future Extensions
- **Volume profile overlay**: Combine with candlesticks
- **Order flow analysis**: Show market maker vs taker flow
- **Depth visualization**: 3D-style order book rendering
- **Custom timeframes**: User-defined tick/time groupings

## Dependencies
- **Existing trade data pipeline**: Already functional
- **Qt Quick rendering**: Current QSGNode architecture
- **Coordinate system**: World→screen transformation system
- **DataProcessor**: Current data aggregation system

## Deliverables
1. **Extended CellInstance** with OHLC and trade data
2. **Tick accumulation logic** in DataProcessor
3. **Proper candlestick rendering** in CandleStrategy
4. **Enhanced trade bubbles** in TradeFlowStrategy
5. **Mode switching UI** controls
6. **Documentation** and usage examples

## Branch Strategy
- **Current branch**: Complete four dirty flags refactor
- **New feature branch**: `feature/candlesticks-trade-bubbles`
- **Incremental implementation**: Phase-by-phase commits
- **Testing**: Verify each mode works independently and in combination

This implementation will transform Sentinel from a heatmap-only tool into a comprehensive market microstructure visualization platform, revealing the algorithmic patterns and trade flow that drive price discovery.