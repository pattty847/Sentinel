# 🎯 QML Component Refactor Blueprint

## Executive Summary

This blueprint provides a comprehensive guide for refactoring the monolithic `DepthChartView.qml` (698 lines) into a clean, feature-based component architecture. The refactor aligns with the V2 modular renderer architecture and resolves mouse event conflicts.

## 🔍 Current State Analysis

### **Current DepthChartView.qml Structure**
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

### **Problems Identified**
1. **Single Responsibility Violation**: One file handles 7+ distinct responsibilities
2. **Maintainability Issues**: Hard to find and fix specific functionality
3. **Reusability Problems**: Controls can't be used in other charts
4. **Mouse Event Conflicts**: QML MouseArea vs C++ mouse events
5. **Testing Complexity**: Can't test components independently
6. **Performance**: Entire file re-renders on any change

## 🎯 Target Architecture: Feature-Based Components

### **Proposed Directory Structure**
```
libs/gui/qml/
├── charts/
│   ├── TradingChart.qml (Main Orchestrator - ~100 lines)
│   ├── ChartArea.qml (Chart + Grid - ~80 lines)
│   └── GridOverlay.qml (Grid lines only - ~60 lines)
├── axes/
│   ├── PriceAxis.qml (Y-axis component - ~120 lines)
│   └── TimeAxis.qml (X-axis component - ~140 lines)
├── controls/
│   ├── NavigationControls.qml (Zoom/Pan - ~80 lines)
│   ├── TimeframeSelector.qml (Timeframe buttons - ~100 lines)
│   ├── VolumeFilter.qml (Volume slider - ~80 lines)
│   └── GridResolutionSelector.qml (Grid controls - ~60 lines)
└── interactions/
    └── KeyboardShortcuts.qml (Keyboard handling - ~40 lines)
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

## 📋 Migration Strategy: Gradual & Safe

### **Phase 1: Extract Controls** (Low Risk - 1-2 days)
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

### **Phase 2: Extract Axes** (Medium Risk - 2-3 days)
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

4. **Update main file** to use axis components
   ```qml
   Rectangle {
       PriceAxis {
           id: priceAxis
           chartRenderer: unifiedGridRenderer
       }
       
       TimeAxis {
           id: timeAxis  
           chartRenderer: unifiedGridRenderer
       }
   }
   ```

**Testing**: Verify coordinate calculations remain accurate

### **Phase 3: Extract Chart & Grid** (Medium Risk - 2-3 days)
**Goal**: Separate chart rendering from UI

1. **Create `/charts/` directory**
2. **Extract ChartArea.qml**
   ```qml
   // ChartArea.qml  
   Item {
       property alias renderer: gridRenderer
       
       UnifiedGridRenderer {
           id: gridRenderer
           objectName: "unifiedGridRenderer"
           // Core rendering properties only
       }
       
       GridOverlay {
           target: gridRenderer
           visible: showGrid
       }
   }
   ```

3. **Extract GridOverlay.qml** with grid line logic

4. **Create main TradingChart.qml** orchestrator
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

**Testing**: Verify data flow and rendering pipeline

### **Phase 4: Final Integration** (Low Risk - 1 day)
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

## 🚨 Risk Mitigation

### **High Priority Risks**
1. **Data Flow Disruption**: Mitigated by preserving UnifiedGridRenderer interface
2. **Mouse Event Conflicts**: Already fixed with spatial boundaries
3. **Performance Regression**: Mitigated by gradual migration and testing

### **Rollback Strategy**
- Each phase is reversible
- Keep original DepthChartView.qml until Phase 4
- Git branch per phase for easy rollback

## 📊 Success Metrics

### **Code Quality**
- ✅ Single Responsibility: Each component has one clear purpose
- ✅ File Size: No component > 150 lines 
- ✅ Reusability: Controls work in multiple contexts
- ✅ Testability: Each component independently testable

### **Performance**
- ✅ FPS: Maintain current rendering performance
- ✅ Memory: No significant memory increase
- ✅ Startup: No loading time regression

### **Maintainability**  
- ✅ Bug Fixes: Easier to locate and fix issues
- ✅ Feature Addition: New controls easy to add
- ✅ Code Navigation: Clear file structure

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

### **Phase 1: Controls** 
- [ ] Create `/controls/` directory
- [ ] Extract NavigationControls.qml
- [ ] Extract TimeframeSelector.qml  
- [ ] Extract VolumeFilter.qml
- [ ] Extract GridResolutionSelector.qml
- [ ] Update DepthChartView.qml to use components
- [ ] Test all control functionality

### **Phase 2: Axes**
- [ ] Create `/axes/` directory
- [ ] Extract PriceAxis.qml with calculation logic
- [ ] Extract TimeAxis.qml with formatting logic
- [ ] Update main file to use axes
- [ ] Test coordinate accuracy

### **Phase 3: Chart & Grid**
- [ ] Create `/charts/` directory
- [ ] Extract ChartArea.qml
- [ ] Extract GridOverlay.qml
- [ ] Create TradingChart.qml orchestrator
- [ ] Test rendering pipeline

### **Phase 4: Integration**
- [ ] Update MainWindowGPU.cpp
- [ ] Add KeyboardShortcuts.qml
- [ ] Delete old DepthChartView.qml
- [ ] Final testing

## 📖 Next Steps

1. **Review this blueprint** with the team
2. **Start with Phase 1** (safest, highest value)
3. **Test thoroughly** after each phase
4. **Iterate and improve** component interfaces
5. **Document lessons learned** for future refactors

---

**This blueprint ensures a safe, gradual migration from monolithic to modular QML architecture while preserving all existing functionality and performance.**