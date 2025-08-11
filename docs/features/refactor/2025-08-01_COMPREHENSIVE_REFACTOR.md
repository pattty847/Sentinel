# 🎯 Sentinel Comprehensive Refactor Blueprint (2025-08-01)

## Executive Summary

This comprehensive blueprint provides a unified guide for refactoring Sentinel's architecture across both QML components and C++ renderer systems. The refactor addresses the monolithic `DepthChartView.qml` (698 lines) and `UnifiedGridRenderer` (~1.5k LOC) by breaking them into composable, test-friendly units while maintaining performance gates and preserving the V2 modular renderer architecture.

## 🔍 Current State Analysis

### **QML Architecture Issues**
```
DepthChartView.qml (698 lines) - God File
├── Chart Rendering (UnifiedGridRenderer) - Lines 47-70
├── Grid Line Overlays (Complex logic) - Lines 73-120  
├── Price Axis (Complex calculations) - Lines 123-221
├── Time Axis (Complex calculations) - Lines 224-365
├── Control Panel (Multiple responsibilities) - Lines 368-672
│   ├── Zoom Controls - Lines 389-431
│   ├── Timeframe Selector - Lines 441-506
│   ├── Price Resolution Slider - Lines 507-576
│   ├── Volume Filter - Lines 578-647
│   └── Grid Resolution - Lines 649-672
└── Keyboard Shortcuts - Lines 676-697
```

### **C++ Renderer Architecture Issues**
| File                          | LOC   | Responsibility                          |
| ----------------------------- | ----- | --------------------------------------- |
| UnifiedGridRenderer.cpp       | 1 576 | Rendering, viewport math, data plumbing |
| UnifiedGridRenderer.h         | 327   | Public API, state, Qt props             |
| LiquidityTimeSeriesEngine.cpp | 403   | Snapshot aggregation, anti‑spoof logic  |
| LiquidityTimeSeriesEngine.h   | 162   | Data structures, interface              |
| CoordinateSystem.*           | 115   | World↔Screen transforms                 |

*Total libs/ footprint: **6 997 LOC / ~64 k tokens**.*

### **Problems Identified**
1. **Single Responsibility Violation**: Both QML and C++ have god classes handling multiple concerns
2. **Maintainability Issues**: Hard to find and fix specific functionality
3. **Reusability Problems**: Components can't be used in other contexts
4. **Mouse Event Conflicts**: QML MouseArea vs C++ mouse events
5. **Testing Complexity**: Can't test components independently
6. **Performance**: Entire systems re-render on any change

## 🎯 Target Architecture: Modular & Composable

### **Proposed Directory Structure**
```
libs/
├── core/          # WebSocket, caches, guards
├── pipeline/      # Ingest → dist → slice → metrics
│   ├── collector/          # SnapshotCollector (+ tests)
│   └── aggregator/         # TimeframeAggregator (+ tests)
├── render/
│   ├── scene/              # Qt SG nodes & materials
│   ├── strategies/         # HeatmapMode, TradeFlowMode, VolumeCandlesMode
│   └── state/              # GridViewState, RenderDiagnostics
├── ui/            # QML & widget glue
│   └── qml/
│       ├── charts/
│       │   ├── TradingChart.qml (Main Orchestrator - ~100 lines)
│       │   ├── ChartArea.qml (Chart + Grid - ~80 lines)
│       │   └── GridOverlay.qml (Grid lines only - ~60 lines)
│       ├── axes/
│       │   ├── PriceAxis.qml (Y-axis component - ~120 lines)
│       │   └── TimeAxis.qml (X-axis component - ~140 lines)
│       ├── controls/
│       │   ├── NavigationControls.qml (Zoom/Pan - ~80 lines)
│       │   ├── TimeframeSelector.qml (Timeframe buttons - ~100 lines)
│       │   ├── VolumeFilter.qml (Volume slider - ~80 lines)
│       │   └── GridResolutionSelector.qml (Grid controls - ~60 lines)
│       └── interactions/
│           └── KeyboardShortcuts.qml (Keyboard handling - ~40 lines)
└── bridge/        # GridIntegrationAdapter (legacy ↔ new)
```

## 🚀 Data Flow Architecture (Preserved)

### **Current Data Pipeline** (✅ Keep Intact)
```
WebSocket → StreamController → GridIntegrationAdapter → UnifiedGridRenderer → V2 Render System
```

### **QML Component Integration**
```
TradingChart.qml (Orchestrator)
├── Exposes: chartRenderer property (UnifiedGridRenderer)
├── ChartArea.qml
│   └── Contains: UnifiedGridRenderer instance
├── Controls/
│   └── Target: chartRenderer (via property binding)
└── Axes/
    └── Source: chartRenderer.viewport* properties
```

## 🖱️ Mouse Event Architecture (Fixed)

### **Spatial Boundaries** (Implemented)
```cpp
// In UnifiedGridRenderer::mousePressEvent()
bool inControlPanel = (pos.x() > width() * 0.75 && pos.y() < height() * 0.65);
bool inPriceAxis = (pos.x() < 60);
bool inTimeAxis = (pos.y() > height() - 30);

if (inControlPanel || inPriceAxis || inTimeAxis) {
    event->ignore();  // Let QML handle
    return;
}
// Handle chart interaction in C++
```

### **Event Responsibility Matrix**
| Area | Handler | Purpose |
|------|---------|---------|
| Control Panel | QML MouseArea | UI interactions → Q_INVOKABLE methods |
| Price Axis | QML MouseArea | UI interactions (future axis controls) |
| Time Axis | QML MouseArea | UI interactions (future axis controls) |
| Chart Area | C++ Mouse Events | Pan/Zoom → GridViewState |

## 🧩 C++ Module Decomposition

### 1. UnifiedGridRenderer ➡️ `render/`

| Concern                   | New Class                                                                                     | Notes                                                          |
| ------------------------- | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------- |
| Scene‑graph orchestration | `GridSceneNode`                                                                               | Owns the `QSGTransformNode` root — no business logic.          |
| Rendering strategy        | `IRenderStrategy` + concrete modes (`HeatmapStrategy`, `TradeFlowStrategy`, `CandleStrategy`) | Classic Strategy pattern; each returns a populated `QSGNode*`. |
| View state & gestures     | `GridViewState`                                                                               | Holds viewport, zoom, pan; emits signals.                      |
| Debug / Perf overlay      | `RenderDiagnostics`                                                                           | Pulls from shared `PerformanceMonitor`.                        |

### 2. LiquidityTimeSeriesEngine ➡️ `pipeline/`

| Concern                 | New Unit              | Notes                                                   |
| ----------------------- | --------------------- | ------------------------------------------------------- |
| Raw snapshot intake     | `SnapshotCollector`   | Depth‑limited, viewport‑aware, 100 ms timer lives here. |
| Time‑bucket aggregation | `TimeframeAggregator` | Produces immutable `LiquiditySlice` objects.            |
| Metrics & anti‑spoof    | `PersistenceAnalyzer` | Calculates `persistenceRatio`, etc.                     |

## 🔌 Shared Interfaces (header‑only)

```cpp
struct Trade { /* existing */ };
struct OrderBook { /* existing */ };

class IOrderFlowSink {
  virtual void onTrade(const Trade&) = 0;
  virtual void onOrderBook(const OrderBook&) = 0;
};

class IRenderStrategy {
  virtual QSGNode* buildNode(const GridSliceBatch& batch,
                             const GridViewState& view) = 0;
  virtual ~IRenderStrategy() = default;
};
```

These seams let us unit‑test aggregation without Qt and swap render modes at runtime.

## 📋 Migration Strategy: Gradual & Safe

### **Phase 1: QML Controls Extraction** (Low Risk - 1-2 days)
**Goal**: Extract reusable control components

1. **Create `/controls/` directory**
2. **Extract NavigationControls.qml**
   ```qml
   // NavigationControls.qml
   Row {
       property UnifiedGridRenderer target
       
       Button { 
           text: "+"
           onClicked: target.zoomIn()
       }
       // ... other controls
   }
   ```

3. **Extract TimeframeSelector.qml**
   ```qml
   // TimeframeSelector.qml
   Column {
       property UnifiedGridRenderer target
       property int currentTimeframe: target.timeframeMs
       
       Row {
           Button { 
               text: "100ms"
               highlighted: currentTimeframe === 100
               onClicked: target.setTimeframe(100)
           }
           // ... other timeframes
       }
   }
   ```

4. **Extract VolumeFilter.qml** & **GridResolutionSelector.qml**

5. **Update DepthChartView.qml** to use new components
   ```qml
   Column {
       NavigationControls { target: unifiedGridRenderer }
       TimeframeSelector { target: unifiedGridRenderer }
       VolumeFilter { target: unifiedGridRenderer }
       GridResolutionSelector { target: unifiedGridRenderer }
   }
   ```

**Testing**: Each component can be tested independently

### **Phase 2: C++ Renderer Extraction** (Medium Risk - 2-3 days)
**Goal**: Extract renderer components while preserving interface

1. **Create render strategy interfaces**
2. **Extract GridViewState** for viewport management
3. **Extract GridSceneNode** for scene graph orchestration
4. **Update UnifiedGridRenderer** to delegate to new components
5. **Preserve QML interface** during transition

### **Phase 3: QML Axes & Chart Extraction** (Medium Risk - 2-3 days)
**Goal**: Modularize complex axis calculation logic

1. **Create `/axes/` directory**
2. **Extract PriceAxis.qml**
   ```qml
   // PriceAxis.qml
   Rectangle {
       property UnifiedGridRenderer chartRenderer
       
       // Move all price calculation logic here
       function calculatePriceTicks() { 
           // Existing logic from DepthChartView
       }
       
       Repeater {
           model: calculatePriceTicks()
           // Price tick rendering
       }
   }
   ```

3. **Extract TimeAxis.qml** with complex time formatting logic

4. **Create ChartArea.qml** and **GridOverlay.qml**

5. **Create main TradingChart.qml** orchestrator
   ```qml
   // TradingChart.qml (New main file)
   Rectangle {
       property alias chartRenderer: chartArea.renderer
       
       ChartArea {
           id: chartArea
           anchors.fill: parent
           anchors.leftMargin: priceAxis.width
           anchors.bottomMargin: timeAxis.height
       }
       
       PriceAxis { 
           id: priceAxis
           chartRenderer: chartArea.renderer 
       }
       
       TimeAxis { 
           id: timeAxis
           chartRenderer: chartArea.renderer 
       }
       
       Column {
           NavigationControls { target: chartArea.renderer }
           TimeframeSelector { target: chartArea.renderer }
           // ... other controls
       }
       
       KeyboardShortcuts { target: chartArea.renderer }
   }
   ```

### **Phase 4: Pipeline Extraction** (Medium Risk - 2-3 days)
**Goal**: Extract data processing pipeline

1. **Create `/pipeline/` directory**
2. **Extract SnapshotCollector** from LiquidityTimeSeriesEngine
3. **Extract TimeframeAggregator** for data aggregation
4. **Extract PersistenceAnalyzer** for anti-spoof logic
5. **Update data flow** to use new pipeline components

### **Phase 5: Final Integration** (Low Risk - 1 day)
**Goal**: Clean up and optimize

1. **Update MainWindowGPU.cpp** to load new main file
   ```cpp
   // Change QML source from DepthChartView.qml to TradingChart.qml
   m_gpuChart->setSource(QUrl("qrc:/qml/TradingChart.qml"));
   ```

2. **Add KeyboardShortcuts.qml** component

3. **Delete old DepthChartView.qml**

4. **Final testing and optimization**

## 🎯 Component Interface Definitions

### **ChartArea.qml Interface**
```qml
Item {
    // Public Properties
    property alias renderer: gridRenderer
    property bool showGrid: true
    
    // Public Methods  
    function resetView() { renderer.resetZoom() }
    
    // Signals
    signal chartClicked(point position)
}
```

### **Control Components Interface**
```qml
// Standard interface for all controls
QtObject {
    property UnifiedGridRenderer target  // Required
    property bool enabled: true
    
    // Signals
    signal valueChanged(var newValue)
}
```

### **Axis Components Interface**
```qml
Rectangle {
    property UnifiedGridRenderer chartRenderer  // Required
    property string axisType: "price" | "time"
    
    // Public Methods
    function refreshTicks()
    function formatValue(value)
}
```

## 🔧 Testing Strategy

### **Unit Testing (Per Phase)**
```qml
// TestNavigationControls.qml
Item {
    NavigationControls {
        id: controls
        target: mockRenderer  // Mock UnifiedGridRenderer
    }
    
    // Test zoom in/out clicks
    // Test button states
    // Test property bindings
}
```

### **Integration Testing**
```qml
// TestTradingChart.qml  
TradingChart {
    id: chart
    
    // Test data flow
    // Test mouse event boundaries  
    // Test component communication
}
```

### **Performance Testing**
- **Render Performance**: Measure FPS before/after refactor
- **Memory Usage**: Verify no memory leaks from component changes
- **Load Time**: Ensure component loading doesn't impact startup

## 🚦 Migration Roadmap & Cursor Prompts

| Phase | Goal                                                        | Cursor Agent Prompt                                                                                    |
| ----- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| 0     | Safety‑net tests on current code                            | `//agent:tests scope=libs goal="Snapshot baseline perf & unit tests"`                                  |
| 1     | QML Controls extraction                                     | `//agent:refactor scope=libs/gui/qml goal="Extract control components from DepthChartView"`             |
| 2     | C++ Renderer extraction                                     | `//agent:refactor scope=libs/gui/UnifiedGridRenderer.* goal="Extract IRenderStrategy & GridViewState"` |
| 3     | QML Axes & Chart extraction                                 | `//agent:refactor scope=libs/gui/qml goal="Extract axis and chart components"`                         |
| 4     | Pipeline extraction                                         | `//agent:refactor scope=libs/core goal="Extract pipeline components from LiquidityTimeSeriesEngine"`   |
| 5     | Swap runtime to modular pieces behind feature flag          | `//agent:structure scope=libs render view="call graph"`                                                |
| 6     | Delete legacy paths, tighten perf                           | `//agent:cleanup scope=libs goal="Remove deprecated members & macros"`                                 |

## 🛡️ Adapter Decision (2025‑08‑01)

We'll **keep `UnifiedGridRenderer` as a thin QML‑facing adapter** for the foreseeable future. The class now:

* Retains all `Q_PROPERTY` and signal surface expected by existing QML.
* Delegates *all* logic to V2 components (`GridViewState`, `GridSceneNode`, strategies).
* Holds **no** viewport state of its own.

Future deprecation path remains open: once V2 is battle‑tested we can mark `UnifiedGridRenderer` as legacy and expose `GridViewState` + `GridSceneNode` directly to QML.

## 🚨 Risk Mitigation

### **High Priority Risks**
1. **Data Flow Disruption**: Mitigated by preserving UnifiedGridRenderer interface
2. **Mouse Event Conflicts**: Already fixed with spatial boundaries
3. **Performance Regression**: Mitigated by gradual migration and testing

### **Rollback Strategy**
- Each phase is reversible
- Keep original files until final phase
- Git branch per phase for easy rollback

## 📊 Success Metrics

### **Code Quality**
- ✅ Single Responsibility: Each component has one clear purpose
- ✅ File Size: No component > 150 lines 
- ✅ Reusability: Components work in multiple contexts
- ✅ Testability: Each component independently testable

### **Performance**
- ✅ FPS: Maintain current rendering performance (≥59 FPS @4K)
- ✅ Memory: No significant memory increase (≤+3 MB vs baseline)
- ✅ Startup: No loading time regression

### **Maintainability**  
- ✅ Bug Fixes: Easier to locate and fix issues
- ✅ Feature Addition: New components easy to add
- ✅ Code Navigation: Clear file structure

## ✅ Acceptance Criteria

* 60 FPS @4 K with 20 k msg/s fire‑hose.
* Memory delta ≤ +3 MB vs baseline.
* Unit coverage ≥ 70 % on pipeline & renderer strategies.
* Debug overlay toggles via `GridSceneNode::setDiagnosticsVisible(true)`.

## 🔄 Future Extensibility

### **Easy Additions**
```qml
// New controls follow standard interface
OrderBookControls { target: chartRenderer }
IndicatorSelector { target: chartRenderer }
AlertControls { target: chartRenderer }
```

### **Multiple Chart Support**
```qml
// Reuse components across chart types
CandlestickChart {
    PriceAxis { chartRenderer: candleRenderer }
    NavigationControls { target: candleRenderer }
}
```

## ✅ Implementation Checklist

### **Phase 1: QML Controls** 
- [ ] Create `/controls/` directory
- [ ] Extract NavigationControls.qml
- [ ] Extract TimeframeSelector.qml  
- [ ] Extract VolumeFilter.qml
- [ ] Extract GridResolutionSelector.qml
- [ ] Update DepthChartView.qml to use components
- [ ] Test all control functionality

### **Phase 2: C++ Renderer**
- [ ] Create render strategy interfaces
- [ ] Extract GridViewState
- [ ] Extract GridSceneNode
- [ ] Update UnifiedGridRenderer to delegate
- [ ] Test rendering pipeline

### **Phase 3: QML Axes & Chart**
- [ ] Create `/axes/` directory
- [ ] Extract PriceAxis.qml with calculation logic
- [ ] Extract TimeAxis.qml with formatting logic
- [ ] Create ChartArea.qml and GridOverlay.qml
- [ ] Create TradingChart.qml orchestrator
- [ ] Test coordinate accuracy

### **Phase 4: Pipeline**
- [ ] Create `/pipeline/` directory
- [ ] Extract SnapshotCollector
- [ ] Extract TimeframeAggregator
- [ ] Extract PersistenceAnalyzer
- [ ] Test data processing pipeline

### **Phase 5: Integration**
- [ ] Update MainWindowGPU.cpp
- [ ] Add KeyboardShortcuts.qml
- [ ] Delete old DepthChartView.qml
- [ ] Final testing

## 🔜 Next Actions

1. Run the **V1‑Purge** agent prompt (see chat).
2. Validate mouse pan/drag in the demo scene.
3. If pan still fails, instrument `ViewState::handlePanMove()` with log lines and dump the coordinate deltas for a single drag so we can pinpoint the math bug.

### Quick sanity script (optional)

```bash
# From repo root
rg -n "m_visibleTimeStart_ms|m_isDragging|createHeatmapNode" ui/ qml/
```

Nothing should pop after the purge.

### Next agent after purge

If the click-drag still misbehaves, feed Cursor something like:

```
📌 Context
  • Drag deltas logged: start (-120, 0) → move (-118, 0) → release (-118, 0)
  • View doesn't move; expect ~2-cell pan.

🎯 Task
  1. Inspect ViewState::handlePanMove() math against GridSceneNode bounds.
  2. Fix incorrect pixel→price/time conversion.
  3. Re-run unit & UI tests.
```

## 📖 Next Steps

1. **Review this blueprint** with the team
2. **Start with Phase 1** (safest, highest value)
3. **Test thoroughly** after each phase
4. **Iterate and improve** component interfaces
5. **Document lessons learned** for future refactors

---

**This comprehensive blueprint ensures a safe, gradual migration from monolithic to modular architecture across both QML and C++ systems while preserving all existing functionality and performance.**
