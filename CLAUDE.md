# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Sentinel is a high-performance real-time financial charting application for cryptocurrency market analysis. It features GPU-accelerated rendering, real-time WebSocket data streaming, and professional-grade liquidity visualization comparable to Bloomberg Terminal and Bookmap.

**Key Technologies**: C++20, Qt6, OpenGL, WebSocket, GPU acceleration
**Architecture**: Modular V2 Grid-Based Architecture with strategy pattern rendering
**Performance**: 2.27M trades/sec processing capacity, sub-millisecond latency

## Build System

### Quick Build Commands

**Configure:**
```bash
# macOS (current platform)
cmake --preset mac-clang

# Windows
cmake --preset windows-mingw

# Linux  
cmake --preset linux-gcc
```

**Build:**
```bash
# macOS (current platform)
cmake --build --preset mac-clang

# Windows
cmake --build --preset windows-mingw

# Linux
cmake --build --preset linux-gcc
```

### Test Commands

**Run all tests:**
```bash
cd build-mac-clang
ctest --output-on-failure
```

**Run specific test suites:**
```bash
# Professional hedge fund validation tests
cmake --build . --target hedge_fund_validation

# Performance validation tests  
cmake --build . --target verify_metrics

# All professional tests
cmake --build . --target professional_tests
```

**Run individual tests:**
```bash
./tests/test_coordinate_system
./tests/test_data_integrity
./tests/test_stress_performance
./tests/test_professional_requirements
./tests/test_order_book_state
```

## Code Architecture

### Directory Structure

```
libs/
├── core/                                    # Core business logic (C++ only)
│   ├── LiquidityTimeSeriesEngine.{h,cpp}   # Multi-timeframe data aggregation
│   ├── MarketDataCore.{hpp,cpp}             # WebSocket networking
│   ├── DataCache.{hpp,cpp}                  # Thread-safe data storage
│   ├── CoinbaseStreamClient.{hpp,cpp}       # WebSocket client implementation
│   ├── TradeData.h                          # Data structures
│   └── PerformanceMonitor.{h,cpp}           # Performance tracking
├── gui/                                     # Qt/QML GUI components
│   ├── UnifiedGridRenderer.{h,cpp}          # Main QML adapter (thin layer)
│   ├── MainWindowGpu.{h,cpp}                # Application window
│   ├── models/                              # QML data models
│   │   ├── TimeAxisModel.{hpp,cpp}          # Time axis calculations
│   │   └── PriceAxisModel.{hpp,cpp}         # Price axis calculations
│   ├── qml/                                 # QML UI components
│   │   ├── DepthChartView.qml               # Main chart view
│   │   └── controls/                        # Extracted control components
│   │       ├── NavigationControls.qml       # Zoom/pan buttons
│   │       ├── TimeframeSelector.qml        # Timeframe switching
│   │       ├── VolumeFilter.qml             # Volume filtering
│   │       ├── GridResolutionSelector.qml   # Grid density controls
│   │       └── PriceResolutionSelector.qml  # Price increment controls
│   └── render/                              # V2 Modular rendering system
│       ├── DataProcessor.{hpp,cpp}          # Data pipeline orchestrator
│       ├── GridViewState.{hpp,cpp}          # UI state & viewport management
│       ├── GridSceneNode.{hpp,cpp}          # GPU scene graph root
│       ├── IRenderStrategy.{hpp,cpp}        # Strategy pattern interface
│       ├── RenderDiagnostics.{hpp,cpp}      # Performance monitoring
│       └── strategies/                      # Pluggable rendering strategies
│           ├── HeatmapStrategy.{hpp,cpp}    # Liquidity heatmap rendering
│           ├── TradeFlowStrategy.{hpp,cpp}  # Trade flow visualization
│           └── CandleStrategy.{hpp,cpp}     # Candlestick chart rendering
apps/
├── sentinel_gui/                            # Main GUI application
└── stream_cli/                              # CLI streaming tool
tests/                                       # Comprehensive test suite
├── test_coordinate_system.cpp               # Coordinate transformation tests
├── test_data_integrity.cpp                 # Data validation tests
├── test_stress_performance.cpp             # Performance validation
├── test_professional_requirements.cpp       # Professional-grade tests
└── test_order_book_state.cpp               # Order book state tests
```

### V2 Modular Architecture

**Design Philosophy**: The codebase uses a clean separation of concerns with modular components:

1. **UnifiedGridRenderer** - Thin QML adapter with zero business logic, delegates everything to V2 components
2. **GridViewState** - Single source of truth for UI state and viewport management
3. **DataProcessor** - Orchestrates all data processing and pipeline coordination
4. **IRenderStrategy** - Strategy pattern for different visualization modes (Heatmap, TradeFlow, Candles)
5. **GridSceneNode** - GPU scene graph management with hardware acceleration

**Key Principle**: Each component has a single, well-defined responsibility and clear interfaces.

### Data Flow Pipeline

```
WebSocket → MarketDataCore → UnifiedGridRenderer → DataProcessor → LiquidityTimeSeriesEngine → IRenderStrategy → GridSceneNode → GPU
```

**Critical Architecture Notes**:
- MarketDataCore must be created BEFORE connecting signals (timing-dependent)
- All UI state flows through GridViewState for consistency
- Strategy pattern allows pluggable visualization modes without code changes
- GPU rendering uses Qt Scene Graph with triple buffering

## Common Development Commands

### Logging System

The project uses a simplified, powerful logging system based on four categories. Control the log output using the `QT_LOGGING_RULES` environment variable.

**Log Categories:**
- `sentinel.app`: Application lifecycle, config, auth.
- `sentinel.data`: Network, cache, trades, market data.
- `sentinel.render`: All rendering, charts, GPU, coordinates.
- `sentinel.debug`: High-frequency diagnostics (off by default).

**Common Commands:**

```bash
# Clean, production-like output (only warnings/errors)
export QT_LOGGING_RULES="*.debug=false"

# Enable all logs for general development
export QT_LOGGING_RULES="sentinel.*.debug=true"

# Debug rendering issues
export QT_LOGGING_RULES="sentinel.render.debug=true"

# Debug data and network issues
export QT_LOGGING_RULES="sentinel.data.debug=true"

# Then run the application
./build-mac-clang/apps/sentinel_gui/sentinel_gui
```

### Performance Monitoring

The application includes built-in performance monitoring:
- Real-time FPS tracking
- GPU memory usage monitoring  
- Cache hit/miss ratios
- Render time analysis
- Network latency tracking

Access via QML properties on UnifiedGridRenderer or through RenderDiagnostics component.

### Code Quality

**Architecture Boundaries**:
- `libs/core/` - Pure C++ business logic, no Qt dependencies except QtCore
- `libs/gui/` - Qt/QML integration layer, delegates to core components
- Clean separation prevents architectural violations

**Performance Requirements**:
- 2.27M trades/sec processing capacity
- Sub-millisecond UI interaction latency
- 60+ FPS at 4K resolution
- Bounded memory usage (~213MB total)

**Thread Safety**:
- Worker thread for WebSocket networking
- GUI thread for Qt UI and GPU rendering
- Lock-free SPSC queues for data transfer
- shared_mutex for concurrent read access

## Important Notes

### Recent Refactoring (Phase 1-2 Complete)

The codebase recently completed a major refactoring (August 2025) that:
- **ELIMINATED 500+ lines of redundant C++ code** by removing StreamController and GridIntegrationAdapter
- **Enforced clean architectural boundaries** by moving LiquidityTimeSeriesEngine to libs/core/
- **Fixed critical data flow timing bug** in MarketDataCore signal connections
- **Established ultra-direct V2 data pipeline** with minimal forwarding layers

**Critical Fix**: MarketDataCore signals must be connected AFTER calling `subscribe()` and `start()` methods, as the MarketDataCore instance is created lazily.

### Code Style

- **NO COMMENTS** unless explicitly requested
- Modern C++20 features and RAII patterns
- Qt6 property bindings for QML integration
- Strategy pattern for pluggable components
- Lock-free data structures for performance

### Testing

The project has comprehensive test coverage:
- Unit tests for individual components
- Integration tests for data pipeline
- Professional requirements validation
- Performance benchmarking
- Stress testing with high-frequency data

### File Extensions

- `.hpp/.cpp` - Modern C++ headers/implementation
- `.h/.cpp` - Legacy C++ (being migrated to .hpp)
- `.qml` - Qt QML UI components
- `.qsb` - Qt Shader Bytecode files

This is a production-ready, high-performance financial application with professional-grade architecture and comprehensive testing.