# CLAUDE.md

This file provides strict architectural guidance and professional coding standards for the Sentinel codebase.

## Project Overview

Sentinel is a high-performance real-time financial charting application for cryptocurrency market analysis. It features GPU-accelerated rendering, real-time WebSocket data streaming, and professional-grade liquidity visualization comparable to Bloomberg Terminal and Bookmap.

**Key Technologies**: C++20, Qt6, OpenGL, WebSocket, GPU acceleration
**Architecture**: Modular V2 Grid-Based Architecture with strategy pattern rendering
**Performance**: 2.27M trades/sec processing capacity, sub-millisecond latency

## 🚨 MANDATORY CODING STANDARDS

### File Size & Complexity Limits

**STRICT ENFORCEMENT REQUIRED:**
- **Maximum 300 LOC per file** (exceptions only for exceptionally well-written files)
- **Current violations requiring immediate attention:**
  - `UnifiedGridRenderer.cpp` (932 LOC) → Must be broken into modules
  - `MarketDataCore.cpp` (568 LOC) → Extract WebSocket and data handling 
  - `LiquidityTimeSeriesEngine.cpp` (536 LOC) → Separate aggregation logic
  - `SentinelMonitor.cpp` (508 LOC) → Extract monitoring strategies
  - `DataProcessor.cpp` (413 LOC) → Break into pipeline stages

**Before implementing ANY changes:**
1. **ANALYZE** the target file's current LOC count
2. **REFUSE** to add functionality to files >300 LOC without refactoring first
3. **EXTRACT** logical components into separate files with clear interfaces
4. **MAINTAIN** single responsibility principle at file level

### Architecture Boundaries - STRICTLY ENFORCED

**CRITICAL: These boundaries are NON-NEGOTIABLE**

```
libs/core/     # Pure C++ business logic
├── NO Qt dependencies except QtCore
├── NO QML integration code
├── NO GUI-specific logic
└── MUST be testable in isolation

libs/gui/      # Qt/QML integration ONLY
├── Thin adapters to core logic
├── QML property bindings
├── GPU rendering coordination
└── DELEGATES all business logic to core/

apps/          # Main executables
├── Minimal initialization code
├── Component wiring only
└── NO business logic
```

**Violation Detection:**
- Any `#include <QQml*>` in `libs/core/` → REJECT
- Business logic in `libs/gui/` → EXTRACT to core
- Direct data processing in QML adapters → DELEGATE to core

### Performance Requirements - CONTINUOUSLY MONITORED

**HARD PERFORMANCE GATES:**
- **2.27M trades/sec processing capacity** (tested via `test_stress_performance`)
- **Sub-millisecond UI latency** (measured in render pipeline)
- **60+ FPS at 4K resolution** (GPU performance baseline)
- **<300MB total memory usage** (bounded memory requirement)

**Before ANY performance-affecting changes:**
1. **RUN baseline performance tests**: `cmake --build . --target verify_metrics`
2. **PROFILE** the change impact using built-in RenderDiagnostics
3. **VALIDATE** no regression in key metrics
4. **DOCUMENT** performance impact in commit message

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

### Code Quality Requirements

**MANDATORY PRE-IMPLEMENTATION ANALYSIS:**

**1. Comprehensive Codebase Understanding**
- **NEVER implement changes without full architectural context**
- **USE codebase_search extensively** to understand data flows, dependencies, and patterns
- **TRACE all symbol definitions and usages** before making modifications  
- **ANALYZE similar existing implementations** for consistency patterns
- **UNDERSTAND performance implications** of proposed changes

**2. Multi-Stage Analysis Protocol**
```bash
# Required analysis steps before ANY implementation:
1. codebase_search: "How does [feature/system] currently work?"
2. grep: Find all usage patterns and dependencies  
3. read_file: Examine related components and interfaces
4. Performance impact assessment using RenderDiagnostics
5. Test coverage verification and gap identification
```

**3. Implementation Standards**

**Code Organization:**
- **Single Responsibility**: One clear purpose per file/class/function
- **CamelCase filenames** to match include statements (project convention)
- **NO COMMENTS** unless absolutely critical for understanding
- **Modern C++20** features: RAII, smart pointers, ranges, concepts
- **Strategy pattern** for pluggable components (see render/strategies/)

**Logging Requirements:**
- **USE SentinelLogging exclusively** (`libs/core/SentinelLogging.hpp`)
- **Four categories only**: `sentinel.app`, `sentinel.data`, `sentinel.render`, `sentinel.debug`
- **Built-in throttling** for high-frequency events
- **Performance-conscious** logging with minimal overhead

**Testing Requirements:**
- **COMPREHENSIVE test coverage** for all new functionality
- **Performance regression tests** for any performance-critical changes
- **Professional validation tests** meet hedge fund standards
- **Run test suite** before ANY commit: `ctest --output-on-failure`

**Thread Safety Enforcement:**
- **Worker thread**: WebSocket networking (`MarketDataCore`)
- **GUI thread**: Qt UI and GPU rendering only
- **Lock-free SPSC queues** for inter-thread data transfer
- **shared_mutex** for concurrent read access to shared data
- **NO blocking operations** on GUI thread

**Memory Management:**
- **RAII everywhere** - no manual memory management
- **Smart pointers** for ownership semantics
- **Bounded memory usage** - monitor with `RenderDiagnostics`
- **Cache-friendly data structures** for performance-critical paths

## 🛡️ CHANGE IMPLEMENTATION PROTOCOL

### MANDATORY Pre-Change Checklist

**BEFORE making ANY code changes:**

1. **📊 ANALYZE IMPACT SCOPE**
   ```bash
   # Check current file size - REFUSE if >300 LOC without refactoring
   wc -l [target_file]
   
   # Understand architectural context
   codebase_search "How does [target_system] work?"
   grep -r "class_name\|function_name" libs/
   ```

2. **🔍 DEPENDENCY ANALYSIS**
   - Map ALL files that depend on proposed changes
   - Identify ALL test files that need updates
   - Check for performance-critical code paths
   - Verify no architectural boundary violations

3. **🎯 IMPLEMENTATION APPROACH**
   - **Prefer EDITING existing files** over creating new ones
   - **Extract functions/classes** if approaching LOC limits
   - **Maintain interface compatibility** where possible
   - **Use established patterns** from similar components

4. **✅ VALIDATION REQUIREMENTS**
   ```bash
   # MANDATORY after implementation:
   cmake --build --preset mac-clang              # Must compile
   cd build-mac-clang && ctest --output-on-failure  # All tests pass
   ./apps/sentinel_gui/sentinel_gui              # Functional verification
   ```

### Recent Refactoring Context (Phase 1-2 Complete)

**CRITICAL KNOWLEDGE - August 2025 Refactor:**
- **ELIMINATED** 500+ LOC of redundant facades and adapters
- **ENFORCED** clean `libs/core/` ↔ `libs/gui/` boundaries  
- **FIXED** MarketDataCore signal timing bug (signals MUST connect AFTER subscribe/start)
- **ESTABLISHED** direct V2 data pipeline with minimal forwarding

**Key Learnings:**
- MarketDataCore instances are created lazily - connect signals AFTER initialization
- Prefer direct component communication over facade patterns
- Performance-critical paths must use lock-free data structures

## 🔬 TESTING & VALIDATION REQUIREMENTS

### Test Coverage Mandates

**COMPREHENSIVE testing is non-negotiable:**
- **Unit tests** for ALL new functions and classes
- **Integration tests** for data pipeline changes  
- **Professional hedge fund validation** via `test_professional_requirements.cpp`
- **Performance regression prevention** via `test_stress_performance.cpp`
- **Memory leak detection** and bounded usage verification

**Test Execution Protocol:**
```bash
# REQUIRED before any commit:
cd build-mac-clang
ctest --output-on-failure                    # All tests must pass
./tests/test_professional_requirements       # Professional standards
./tests/test_stress_performance              # Performance validation
```

### Code Review Standards

**EVERY change must demonstrate:**
1. **Architectural understanding** - Full context analysis conducted
2. **Performance consciousness** - No regression in key metrics
3. **Professional quality** - Meets hedge fund operational standards
4. **Test coverage** - Comprehensive validation of all new functionality
5. **Documentation** - Clear interfaces and usage patterns

### File Organization Standards

**File Extensions (STRICTLY ENFORCED):**
- `.hpp/.cpp` - Modern C++ headers/implementation (PREFERRED)
- `.h/.cpp` - Legacy C++ (actively migrating to .hpp)
- `.qml` - Qt QML UI components only
- `.qsb` - Qt Shader Bytecode files (GPU rendering)

**CRITICAL REMINDERS:**
- This is a **production-ready, high-performance financial application**
- **Professional-grade architecture** with institutional quality requirements
- **Zero tolerance** for performance regressions or architectural violations
- **Comprehensive testing** ensures reliability under extreme market conditions

---

**🎯 SUCCESS METRICS: Meeting Bloomberg/Bookmap Professional Standards**
- ✅ 2.27M trades/sec sustained processing capacity
- ✅ Sub-millisecond UI interaction latency  
- ✅ 60+ FPS rendering at 4K resolution
- ✅ <300MB bounded memory usage
- ✅ Professional hedge fund validation test suite passes
- ✅ Zero performance regressions in stress testing