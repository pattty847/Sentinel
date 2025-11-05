# Sentinel Test Suite

Unit tests for the Sentinel trading terminal, using Google Test framework.

## Current Test Coverage

### Market Data Tests (`marketdata/`)

Core data pipeline validation for the V2 architecture:

| Test Suite | File | Coverage |
|------------|------|----------|
| **MessageDispatcher** | `test_message_dispatcher.cpp` | Channel-based message routing, subscription callbacks, trade/orderbook dispatch |
| **SubscriptionManager** | `test_subscription_manager.cpp` | Symbol subscription lifecycle, multiple products, JSON generation |
| **DataCache Sink** | `test_datacache_sink_adapter.cpp` | DataCache integration with dispatcher sinks, trade/orderbook updates |

**Status**: ✅ All 3 test suites passing

### WebSocket Reliability Features

The market data pipeline includes production-grade reliability mechanisms:

**Sequence Number Tracking**
- Every WebSocket message includes a sequence number from Coinbase
- Detects gaps/drops in real-time: missing sequences trigger reconnection
- Prevents silent data loss that could corrupt the order book
- Critical for maintaining book integrity during network instability

**Heartbeat Monitoring**
- Coinbase sends periodic heartbeat messages to confirm connection health
- Watchdog timer detects missing heartbeats (connection dead but not closed)
- Automatic reconnection if heartbeats stop arriving
- Prevents "zombie connections" that appear alive but aren't receiving data

**Result**: Order book stays **tight and accurate** even under:
- Network packet loss
- ISP routing issues
- Exchange-side connection problems
- Brief disconnections

These features were challenging to implement correctly (thanks Codex!) but are essential for production trading systems where stale data = bad decisions.

## Running Tests

### All Tests
```bash
cd build-<platform>
ctest --output-on-failure
```

### Market Data Tests Only
```bash
cd build-<platform>
ctest -R "MessageDispatcher|SubscriptionManager|DataCacheSinkAdapter" --output-on-failure
```

### Individual Test
```bash
cd build-<platform>
./tests/marketdata/test_message_dispatcher
```

## Test Framework

- **Framework**: Google Test (GTest)
- **Build System**: CMake with `enable_testing()`
- **Execution**: CTest for discovery and running
- **Assertions**: Standard GTest macros (`EXPECT_EQ`, `ASSERT_TRUE`, etc.)

## Adding New Tests

### 1. Create Test File

Create a new test file in the appropriate subdirectory:

```cpp
// tests/marketdata/test_new_component.cpp
#include <gtest/gtest.h>
#include "YourComponent.hpp"

TEST(NewComponentTest, BasicFunctionality) {
    // Arrange
    YourComponent component;

    // Act
    auto result = component.doSomething();

    // Assert
    EXPECT_TRUE(result);
}
```

### 2. Update CMakeLists.txt

Add to `tests/marketdata/CMakeLists.txt`:

```cmake
add_executable(test_new_component test_new_component.cpp)
target_include_directories(test_new_component PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)
target_link_libraries(test_new_component PRIVATE
    sentinel_core
    GTest::gtest_main
)
add_test(NAME NewComponentTests COMMAND test_new_component)
```

### 3. Build & Run

```bash
cmake --build --preset <platform>
cd build-<platform>
ctest -R NewComponent --output-on-failure
```

## Future Test Areas

Planned test coverage for upcoming development:

### Rendering (`render/`)
- [ ] `test_heatmap_strategy.cpp` - HeatmapStrategy cell generation, color mapping
- [ ] `test_candle_strategy.cpp` - CandleStrategy OHLC aggregation
- [ ] `test_data_processor.cpp` - Snapshot generation, viewport management
- [ ] `test_grid_view_state.cpp` - Pan/zoom transformations, viewport calculations

### Core Engine (`core/`)
- [ ] `test_liquidity_time_series.cpp` - Multi-timeframe aggregation (100ms to 10s)
- [ ] `test_live_order_book.cpp` - O(1) price level lookups, capacity limits
- [ ] `test_sentinel_monitor.cpp` - Performance metrics, latency tracking

### Integration (`integration/`)
- [ ] `test_end_to_end_pipeline.cpp` - Full data flow from WebSocket to GPU render
- [ ] `test_performance_regression.cpp` - Validate 2,000x paint time improvements hold

## Testing Philosophy

### What We Test
- **Business Logic**: Pure functions, data transformations, algorithms
- **Integration Points**: Component boundaries, signal/slot connections
- **Critical Paths**: Hot paths in rendering and data processing
- **Invariants**: O(1) lookups, memory bounds, thread safety

### What We Don't Test
- **QML UI**: Difficult to unit test; validated through manual testing
- **GPU Rendering**: Integration tested via live application
- **Network I/O**: Tested with mock transports, not live WebSocket
- **Third-Party Libraries**: Trust Qt, Boost, nlohmann::json

### Test Naming Convention

```
TEST(<ComponentName>Test, <Behavior>_<Condition>_<ExpectedResult>)

Examples:
- MessageDispatcherTest, RegisterChannel_NewChannel_ReturnsSuccess
- SubscriptionManagerTest, Subscribe_DuplicateSymbol_IgnoresDuplicate
- DataCacheTest, UpdateTrade_ValidTrade_StoresCorrectly
```

## Clean Build & Test Workflow

Full clean rebuild with test execution:

```bash
# Clean
rm -rf build-<platform>

# Configure
cmake --preset <platform>

# Build
cmake --build --preset <platform> -j$(nproc)

# Test
cd build-<platform>
ctest --output-on-failure

# Expected output:
#   100% tests passed, 0 tests failed out of 3
#   Total Test time (real) =   0.xx sec
```

## Continuous Integration

Tests run automatically on:
- Every push to `main` branch
- All pull requests
- Pre-commit hooks (optional)

**Pass Criteria**: All tests must pass before merge.

## Historical Note

Previous test suite targeted the legacy architecture and has been removed:
- `test_professional_requirements.cpp` - Referenced removed `CoinbaseStreamClient`
- `test_order_book_state.cpp` - Targeted old order book implementation
- `test_data_integrity.cpp` - Legacy data validation approach
- `test_stress_performance.cpp` - Pre-GPU acceleration benchmarks

The new V2 architecture (GPU-accelerated, async snapshots, lock-free pipelines) requires fundamentally different testing approaches. We're building the test suite incrementally as features stabilize.

---

**Last Updated**: 2025-11-03
**Test Count**: 3 suites (MessageDispatcher, SubscriptionManager, DataCacheSinkAdapter)
**Status**: ✅ All passing
