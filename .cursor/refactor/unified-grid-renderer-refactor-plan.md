# UnifiedGridRenderer Refactor Plan

## Current State Analysis

### Mixed Responsibilities Identified

`UnifiedGridRenderer.cpp` currently mixes **9 distinct responsibilities**:

1. **QML/Qt Integration Layer** (Lines 38-51, 145-154, 696-722)
   - QQuickItem lifecycle management
   - Q_PROPERTY getters/setters
   - QML signal emissions
   - Component initialization

2. **Thread Lifecycle Management** (Lines 53-83, 384-405, 437-438)
   - DataProcessor thread creation/destruction
   - Cross-thread signal/slot wiring
   - Thread-safe shutdown coordination

3. **Data Reception & Forwarding** (Lines 85-101, 441-450)
   - Trade reception slots
   - DataCache assignment
   - Cross-thread data forwarding to DataProcessor

4. **Viewport State Coordination** (Lines 105-125, 128-143, 328-344, 346-375)
   - Viewport change handling
   - GridViewState delegation
   - Coordinate system exposure to QML
   - Pan/zoom command forwarding

5. **Rendering Orchestration** (Lines 559-691)
   - Dirty flag management (4 flags: geometry, append, material, transform)
   - updatePaintNode logic with conditional paths
   - Strategy selection and layering
   - GridSceneNode lifecycle

6. **Event Handling** (Lines 726-762)
   - Mouse press/move/release
   - Wheel events
   - Event-to-viewport translation

7. **Configuration Management** (Lines 176-283)
   - Render mode switching
   - Visual parameter setters (intensity, filters, bubble settings)
   - Grid resolution/timeframe management

8. **Data Accessor Factory** (Lines 479-557)
   - UGRDataAccessor construction
   - Thread-local trade cache workaround
   - Viewport snapshot assembly

9. **Volume Profile Management** (Lines 170-173, 608-612)
   - Volume profile calculation (stub)
   - Profile node updates

---

## 3-Phase Refactor Plan

### Phase 1: Extract Responsibilities

**Goal**: Separate concerns into focused components without changing behavior.

#### 1.1 Extract `RenderCoordinator` (New Class)
**Extract from**: Lines 559-691 (updatePaintNode logic)

**Responsibilities**:
- Dirty flag state machine
- Strategy selection and layering coordination
- GridSceneNode lifecycle
- Render path decision logic (geometry vs append vs material vs transform)

**Interface**:
```cpp
class RenderCoordinator {
public:
    enum class UpdateType { Geometry, Append, Material, Transform };
    
    UpdateType determineUpdateType(bool geometryDirty, bool appendPending, 
                                   bool materialDirty, bool transformDirty, bool isNewNode);
    void updateSceneNode(GridSceneNode* node, UpdateType type, 
                        const IDataAccessor* accessor, 
                        IRenderStrategy* heatmap, bool showHeatmap,
                        IRenderStrategy* bubble, bool showBubbles,
                        IRenderStrategy* flow, bool showFlow,
                        const QMatrix4x4& transform);
};
```

**Benefits**:
- Testable render logic without QQuickItem
- Clear separation of "what to render" from "when Qt calls updatePaintNode"
- Easier to optimize render paths independently

**Traps**:
- ⚠️ Must preserve exact dirty flag semantics (atomic exchanges)
- ⚠️ GridSceneNode ownership stays in UnifiedGridRenderer (QSGNode lifecycle)
- ⚠️ IDataAccessor must be constructed before calling updateSceneNode

---

#### 1.2 Extract `DataCoordinator` (New Class)
**Extract from**: Lines 85-101, 441-450, 156-168

**Responsibilities**:
- DataCache assignment and forwarding
- Trade reception and queuing to DataProcessor
- Visible cells snapshot management
- DataProcessor lifecycle coordination

**Interface**:
```cpp
class DataCoordinator : public QObject {
    Q_OBJECT
public:
    explicit DataCoordinator(QObject* parent = nullptr);
    
    void setDataCache(DataCache* cache);
    void setDataProcessor(DataProcessor* processor);
    void onTradeReceived(const Trade& trade);
    
    std::shared_ptr<const std::vector<CellInstance>> getVisibleCellsSnapshot() const;
    
signals:
    void dataUpdated();
};
```

**Benefits**:
- Isolates data flow from rendering
- Testable data coordination without QQuickItem
- Clear contract: "data comes in, snapshots come out"

**Traps**:
- ⚠️ Must preserve QueuedConnection semantics for thread safety
- ⚠️ DataProcessor thread lifecycle still managed by UnifiedGridRenderer
- ⚠️ Snapshot access must remain thread-safe (shared_ptr swap)

---

#### 1.3 Extract `ViewportCoordinator` (New Class)
**Extract from**: Lines 105-125, 346-375, 328-344

**Responsibilities**:
- Viewport change propagation
- Coordinate system QML exposure
- Pan/zoom command forwarding to GridViewState
- Viewport-to-Viewport conversion

**Interface**:
```cpp
class ViewportCoordinator : public QObject {
    Q_OBJECT
public:
    explicit ViewportCoordinator(GridViewState* viewState, QObject* parent = nullptr);
    
    void setViewport(qint64 timeStart, qint64 timeEnd, double minPrice, double maxPrice);
    void onViewportChanged();
    
    QPointF worldToScreen(qint64 timestamp_ms, double price, double width, double height) const;
    QPointF screenToWorld(double screenX, double screenY, double width, double height) const;
    
    // Pan/zoom forwarding
    void zoomIn(double width, double height);
    void zoomOut(double width, double height);
    void resetZoom();
    void panLeft(); void panRight(); void panUp(); void panDown();
    void enableAutoScroll(bool enabled);
    
signals:
    void viewportChanged();
    void panVisualOffsetChanged();
    void autoScrollEnabledChanged();
};
```

**Benefits**:
- Viewport logic testable without QML
- Clear boundary: "viewport state" vs "viewport presentation"
- Easier to mock GridViewState for testing

**Traps**:
- ⚠️ Must preserve viewport version incrementing semantics
- ⚠️ Coordinate system calculations must match exactly (no pan offset double-counting)
- ⚠️ GridViewState signals must still connect to UnifiedGridRenderer for QML properties

---

#### 1.4 Extract `EventHandler` (New Class)
**Extract from**: Lines 726-762

**Responsibilities**:
- Mouse event handling
- Wheel event handling
- Event-to-viewport-action translation

**Interface**:
```cpp
class EventHandler {
public:
    explicit EventHandler(GridViewState* viewState);
    
    bool handleMousePress(QMouseEvent* event, double width, double height);
    bool handleMouseMove(QMouseEvent* event, double width, double height);
    bool handleMouseRelease(QMouseEvent* event);
    bool handleWheel(QWheelEvent* event, double width, double height);
    
    // Returns true if event was consumed
    bool shouldAcceptEvent(QMouseEvent* event) const;
};
```

**Benefits**:
- Event handling testable without QQuickItem
- Can unit test pan/zoom behavior
- Easier to add gesture support later

**Traps**:
- ⚠️ Must preserve exact pan visual offset behavior (m_panSyncPending logic)
- ⚠️ Event acceptance logic must match QML expectations
- ⚠️ update() calls must still happen in UnifiedGridRenderer (Qt requirement)

---

### Phase 2: Isolate Dependencies

**Goal**: Reduce coupling between components and make dependencies explicit.

#### 2.1 Introduce `IRenderCoordinator` Interface
**Purpose**: Allow swapping render coordination strategies

**Interface**:
```cpp
class IRenderCoordinator {
public:
    virtual ~IRenderCoordinator() = default;
    virtual void updatePaintNode(GridSceneNode* node, 
                                const RenderState& state,
                                const IDataAccessor* accessor,
                                StrategySet& strategies) = 0;
};
```

**Benefits**:
- Can test UnifiedGridRenderer with mock coordinator
- Future: A/B test different render strategies
- Clear contract for render updates

---

#### 2.2 Extract Configuration into `RenderConfig` Struct
**Extract from**: Lines 85-104 (header), 176-283 (setters)

**Structure**:
```cpp
struct RenderConfig {
    RenderMode renderMode = RenderMode::LiquidityHeatmap;
    bool showVolumeProfile = true;
    double intensityScale = 1.0;
    int maxCells = 100000;
    double minVolumeFilter = 0.0;
    
    // Bubble settings
    double minBubbleRadius = 4.0;
    double maxBubbleRadius = 20.0;
    double bubbleOpacity = 0.85;
    
    // Layer toggles
    bool showHeatmapLayer = true;
    bool showTradeBubbleLayer = true;
    bool showTradeFlowLayer = false;
    
    // Validation
    bool isValid() const;
};
```

**Benefits**:
- Configuration changes are atomic
- Easier to serialize/deserialize settings
- Clear validation boundaries

**Traps**:
- ⚠️ Must emit individual signals for QML bindings (Q_PROPERTY requirements)
- ⚠️ Setters must still trigger dirty flags

---

#### 2.3 Create `DataAccessorFactory`
**Extract from**: Lines 479-557 (UGRDataAccessor construction)

**Purpose**: Centralize accessor creation logic

**Interface**:
```cpp
class DataAccessorFactory {
public:
    static std::unique_ptr<IDataAccessor> create(
        UnifiedGridRenderer* ugr,
        const RenderConfig& config,
        const Viewport& viewport,
        DataCache* cache,
        DataProcessor* processor,
        const std::string& symbol);
};
```

**Benefits**:
- Isolates thread-local workaround (can be fixed later)
- Testable accessor creation
- Single place to fix dangling reference issue

**Traps**:
- ⚠️ Thread-local static in getRecentTrades() is a known bug - document it
- ⚠️ Must return by value or use shared_ptr to avoid dangling references

---

### Phase 3: Make Testable Units

**Goal**: Enable unit testing of each component independently.

#### 3.1 Mock Dependencies
**Create mocks for**:
- `MockGridViewState` - Test viewport coordination without real state
- `MockDataProcessor` - Test data flow without background thread
- `MockDataCache` - Test data access without real cache
- `MockRenderStrategy` - Test rendering without GPU

**Benefits**:
- Unit tests run fast (no QML, no threads, no GPU)
- Isolated failure points
- Can test edge cases (empty data, invalid viewports, etc.)

---

#### 3.2 Extract Pure Functions
**Identify and extract**:
- `buildViewport()` - Already in anonymous namespace (line 472)
- `calculateOptimalResolution()` - Already static (header line 175)
- Viewport conversion helpers

**Move to**: `libs/gui/render/ViewportUtils.hpp`

**Benefits**:
- Trivially testable
- No side effects
- Can be benchmarked independently

---

#### 3.3 Create Integration Test Harness
**Purpose**: Test UnifiedGridRenderer with real dependencies

**Structure**:
```cpp
class UnifiedGridRendererTestHarness {
    QQuickView m_view;
    UnifiedGridRenderer* m_renderer;
    MockDataCache m_cache;
    // ... setup helpers
public:
    void setupRenderer();
    void simulateTrade(const Trade& trade);
    void simulateViewportChange(qint64 start, qint64 end, double min, double max);
    QSGNode* getSceneNode();
};
```

**Benefits**:
- Tests real QML integration
- Validates thread safety
- Catches integration bugs

---

## Traps & Regressions to Watch

### Critical Threading Traps

1. **DataProcessor Thread Shutdown** (Lines 56-77)
   - ⚠️ **CRITICAL**: `stopProcessing()` must be called via `BlockingQueuedConnection` before thread destruction
   - **Regression**: App hangs on close if thread cleanup is wrong
   - **Test**: Close app during active data stream

2. **Cross-Thread Signal/Slot** (Lines 98-99, 121, 389-400)
   - ⚠️ **CRITICAL**: All DataProcessor calls must use `Qt::QueuedConnection`
   - **Regression**: Data corruption or crashes if direct calls happen
   - **Test**: Rapid trade reception during viewport changes

3. **Snapshot Thread Safety** (Lines 160-162, 509-514)
   - ⚠️ **CRITICAL**: `getPublishedCellsSnapshot()` uses mutex; must not block render thread
   - **Regression**: Frame drops if snapshot access blocks
   - **Test**: High-frequency data updates during rendering

---

### Rendering Traps

4. **Dirty Flag Ordering** (Lines 582, 615, 642, 663)
   - ⚠️ **CRITICAL**: Priority is geometry → append → material → transform
   - **Regression**: Wrong update path = visual glitches or performance issues
   - **Test**: Rapid mode switches + data updates + pan/zoom simultaneously

5. **Viewport Version Gating** (Lines 108, 121-124)
   - ⚠️ **CRITICAL**: Viewport changes must increment version; DataProcessor checks this
   - **Regression**: Stale cells rendered if version doesn't increment
   - **Test**: Pan during data append

6. **Pan Visual Offset Sync** (Lines 392-395, 751-753)
   - ⚠️ **CRITICAL**: Visual pan offset cleared only after DataProcessor resync
   - **Regression**: Snap-back during pan if cleared too early
   - **Test**: Rapid pan gestures

---

### QML Integration Traps

7. **Q_PROPERTY Signal Emissions** (Throughout setters)
   - ⚠️ **CRITICAL**: Every setter must emit corresponding signal for QML bindings
   - **Regression**: QML UI doesn't update if signals missing
   - **Test**: Change properties from QML, verify UI updates

8. **Component Lifecycle** (Lines 145-154)
   - ⚠️ **CRITICAL**: `componentComplete()` sets initial viewport size
   - **Regression**: 0x0 viewport = no rendering
   - **Test**: Renderer initialization with delayed geometry

9. **Coordinate System Pan Offset** (Lines 358, 373)
   - ⚠️ **CRITICAL**: Comments say "NO pan offset" - transform handles it in node
   - **Regression**: Double pan offset if coordinate system applies it
   - **Test**: Pan + coordinate conversion

---

### Data Flow Traps

10. **Dangling Reference in UGRDataAccessor** (Lines 516-539)
    - ⚠️ **KNOWN BUG**: `getRecentTrades()` uses thread_local to work around interface mismatch
    - **Regression**: Dangling reference if DataCache::recentTrades() returns temporary
    - **Fix**: Change IDataAccessor interface to return by value or shared_ptr

11. **Symbol Hardcoding** (Lines 589, 598, 630, 654)
    - ⚠️ **TODO**: Symbol is hardcoded to "BTC-USD"
    - **Regression**: Wrong data shown if symbol changes
    - **Fix**: Get symbol from ViewState or property

12. **Volume Profile Stub** (Lines 170-173)
    - ⚠️ **INCOMPLETE**: `updateVolumeProfile()` is empty
    - **Regression**: Volume profile never updates
    - **Fix**: Implement from LiquidityTimeSeriesEngine

---

## Migration Strategy

### Phase 1 Migration (Low Risk)
1. Create new classes alongside UnifiedGridRenderer
2. Move logic incrementally, keeping old code commented
3. Wire new classes into UnifiedGridRenderer
4. Verify behavior matches exactly
5. Remove old code

### Phase 2 Migration (Medium Risk)
1. Introduce interfaces gradually
2. Keep concrete implementations as defaults
3. Add unit tests for new interfaces
4. Migrate one dependency at a time

### Phase 3 Migration (Higher Risk - Requires Test Infrastructure)
1. Set up test harness first
2. Write integration tests for current behavior
3. Refactor with tests as safety net
4. Add unit tests for new components

---

## Success Criteria

✅ **Phase 1 Complete When**:
- UnifiedGridRenderer.cpp < 400 lines (currently 763)
- Each extracted class has single responsibility
- All existing functionality preserved
- No performance regression

✅ **Phase 2 Complete When**:
- Dependencies are injected, not hardcoded
- Configuration is centralized
- Interfaces are testable
- No QML breakage

✅ **Phase 3 Complete When**:
- Unit tests cover all extracted components
- Integration tests cover UnifiedGridRenderer
- Test coverage > 80% for new code
- All known bugs documented and prioritized

---

## Estimated Effort

- **Phase 1**: 2-3 days (extract 4 classes)
- **Phase 2**: 1-2 days (interfaces + config)
- **Phase 3**: 3-4 days (tests + mocks)
- **Total**: ~1.5 weeks

---

## Notes

- Keep `AGENTS.md` rules: Core stays pure C++, GUI owns Qt
- Preserve performance: No mutex contention, no blocking on render thread
- Maintain QML compatibility: All Q_PROPERTY and signals must work
- Document thread boundaries clearly in each extracted class

