# Sentinel Dockable Framework Architecture

## Overview

The Sentinel trading terminal uses a sophisticated dockable widget framework built on Qt's `QMainWindow` and `QDockWidget` system. This architecture enables a professional trading terminal experience with customizable layouts, persistent state management, and seamless widget integration.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Core Components](#core-components)
3. [Dock Widget System](#dock-widget-system)
4. [Layout Management](#layout-management)
5. [Status Bar System](#status-bar-system)
6. [Service Locator Pattern](#service-locator-pattern)
7. [Signal/Slot Architecture](#signalslot-architecture)
8. [Threading Model](#threading-model)
9. [Implementation Details](#implementation-details)
10. [Future Enhancements](#future-enhancements)

---

## Architecture Overview

### High-Level Structure

```
MainWindowGPU (QMainWindow)
├── Menu Bar (View, Layouts, Tools)
├── Central Widget (StatusDock - minimal placeholder)
├── Dock Widgets (arranged in areas)
│   ├── HeatmapDock (Right area, prominent)
│   │   └── Embedded Symbol Control Bar
│   ├── MarketDataPanel (Left area, tabbed)
│   ├── SecFilingDock (Left area, tabbed)
│   ├── CopenetFeedDock (Bottom area, tabbed)
│   └── AICommentaryFeedDock (Bottom area, tabbed)
└── Status Bar (Bottom, dock-like)
    └── StatusBar widget (Ready, Connection, CPU, GPU, Latency)
```

### Key Design Principles

1. **Separation of Concerns**: Each dock widget is self-contained with its own UI and logic
2. **Modularity**: New docks can be added without modifying existing code
3. **State Persistence**: Layouts are saved/restored automatically
4. **Thread Safety**: All GUI updates happen on the main thread via queued connections
5. **Performance**: GPU rendering stays in separate thread, UI updates are coalesced

---

## Core Components

### 1. DockablePanel (Base Class)

**Location**: `libs/gui/widgets/DockablePanel.hpp/.cpp`

**Purpose**: Abstract base class for all dockable widgets, providing:
- Consistent behavior across all docks
- Symbol change propagation
- Layout restoration support
- Menu integration

**Key Features**:
```cpp
class DockablePanel : public QDockWidget {
    Q_OBJECT
public:
    explicit DockablePanel(const QString& id, const QString& title, QWidget* parent = nullptr);
    virtual void buildUi() = 0;  // Pure virtual - subclasses must implement
    virtual void onSymbolChanged(const QString& symbol) {}  // Hook for symbol updates
protected:
    QString m_panelId;  // Persistent identifier for layout restoration
    QWidget* m_contentWidget;  // Container for dock's UI
};
```

**Mechanism**:
- Uses `setObjectName(id)` for Qt's layout restoration system
- Provides `m_contentWidget` as a standard container
- Enables symbol change propagation via `onSymbolChanged()` virtual method

### 2. MainWindowGPU

**Location**: `libs/gui/MainWindowGpu.h/.cpp`

**Purpose**: Main application window managing all docks, layout, and core services.

**Key Responsibilities**:
- Creates and arranges all dock widgets
- Manages layout save/restore
- Connects market data signals to renderer
- Provides unified `symbolChanged` signal for all docks
- Handles menu bar and shortcuts

**Critical Methods**:
- `setupUI()`: Creates all docks and arranges default layout
- `arrangeDefaultLayout()`: Programmatically resets docks to default positions
- `resetLayoutToDefault()`: Slot called by LayoutManager to reset layout
- `onConnectionStatusChanged()`: Updates status bar when connection changes

---

## Dock Widget System

### Widget Hierarchy

All dock widgets inherit from `DockablePanel`:

```
DockablePanel (base)
├── HeatmapDock (QML GPU rendering + embedded symbol controls)
├── MarketDataPanel (QAbstractTableModel + QTableView)
├── SecFilingDock (SEC data fetching UI)
├── CopenetFeedDock (Commentary feed display)
└── AICommentaryFeedDock (AI commentary display)
```

### HeatmapDock - Special Case

**Why Special**: Contains embedded symbol control bar that stays attached when docked/undocked.

**Implementation**:
```cpp
void HeatmapDock::buildUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(m_contentWidget);
    
    // QML container (takes most space)
    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, m_contentWidget);
    mainLayout->addWidget(m_qmlContainer, 1);
    
    // Embedded symbol control bar (fixed height at bottom)
    QWidget* symbolBar = new QWidget(m_contentWidget);
    symbolBar->setFixedHeight(32);
    // ... symbol input, subscribe button ...
    mainLayout->addWidget(symbolBar, 0);
}
```

**Key Insight**: By embedding the symbol control inside `HeatmapDock`, it becomes part of the dock widget itself. When the heatmap is moved or undocked, the symbol control moves with it automatically - no special docking logic needed.

### Default Layout Arrangement

**Location**: `MainWindowGPU::arrangeDefaultLayout()`

**Strategy**:
1. Remove all docks from current positions
2. Add heatmap to right area (most prominent)
3. Add SEC/MarketData to left area, tabbed together
4. Add commentary feeds to bottom area, tabbed together
5. Set relative sizes using `resizeDocks()`

**Code Flow**:
```cpp
void MainWindowGPU::arrangeDefaultLayout() {
    setUpdatesEnabled(false);  // Prevent repaint storms
    
    // Remove all docks first
    removeDockWidget(m_heatmapDock);
    // ... remove others ...
    
    // Add to areas
    addDockWidget(Qt::RightDockWidgetArea, m_heatmapDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_secDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_marketDataDock);
    tabifyDockWidget(m_secDock, m_marketDataDock);  // Tab them
    
    // Set sizes
    resizeDocks({m_copenetDock, m_aiCommentaryDock}, {1, 1}, Qt::Horizontal);
    
    setUpdatesEnabled(true);
}
```

---

## Layout Management

### LayoutManager

**Location**: `libs/gui/widgets/LayoutManager.hpp/.cpp`

**Purpose**: Handles saving, restoring, and resetting dock layouts using `QSettings`.

**Key Features**:
- Version-aware layout storage (prevents crashes on app updates)
- Named layout support (save multiple layouts)
- Default layout management
- Safe restoration with fallback

**Storage Format**:
```cpp
QSettings settings("Sentinel", "SentinelTerminal");
settings.beginGroup("layouts");
settings.beginGroup(layoutName);
settings.setValue("version", APP_LAYOUT_VERSION);  // Version check
settings.setValue("state", window->saveState());   // Binary layout data
```

**Version Safety**:
```cpp
int savedVersion = settings.value("version", 0).toInt();
if (savedVersion != APP_LAYOUT_VERSION) {
    qWarning() << "Layout version mismatch - falling back to default";
    return false;  // Don't restore incompatible layouts
}
```

**Reset Mechanism**:
```cpp
void LayoutManager::resetToDefault(QMainWindow* window) {
    deleteLayout(defaultLayoutName());  // Clear saved state
    QMetaObject::invokeMethod(window, "resetLayoutToDefault", Qt::QueuedConnection);
}
```

This calls `MainWindowGPU::resetLayoutToDefault()` slot, which programmatically rearranges docks.

### Layout Persistence Flow

1. **On Close**: `MainWindowGPU::closeEvent()` saves current layout
2. **On Startup**: Constructor tries to restore saved layout
3. **On Reset**: User triggers reset → clears saved state → programmatically rearranges
4. **On Save**: User saves named layout → stored with version number
5. **On Restore**: User selects layout → version checked → restored if compatible

---

## Status Bar System

### Architecture

**Two-Level Status Display**:

1. **StatusBar Widget** (`libs/gui/widgets/StatusBar.hpp/.cpp`)
   - Custom widget displaying metrics
   - Embedded in Qt's `QStatusBar`
   - Shows: Ready, Connection, CPU%, GPU%, Latency

2. **QMainWindow Status Bar**
   - Qt's built-in status bar
   - Contains our custom `StatusBar` widget
   - Positioned at bottom of window

### StatusBar Widget

**Layout**:
```
[Ready] ................ [🟢 Connected] [CPU: 45%] [GPU: 32%] [Latency: 12 ms]
```

**Color Coding**:
- **CPU/GPU**: Green < 50%, Yellow < 80%, Red ≥ 80%
- **Latency**: Green < 50ms, Yellow < 100ms, Red ≥ 100ms
- **Connection**: Green when connected, Red when disconnected

**Integration**:
```cpp
// In MainWindowGPU::setupUI()
m_statusBar = new StatusBar(this);
statusBar()->addPermanentWidget(m_statusBar);  // Add to Qt's status bar
statusBar()->setStyleSheet("QStatusBar { background-color: #1e1e1e; border-top: 1px solid #333; }");
```

**Update Mechanism**:
```cpp
void MainWindowGPU::onConnectionStatusChanged(bool connected) {
    if (m_statusBar) {
        m_statusBar->setConnectionStatus(connected);
    }
}
```

### Future Expansion

The status bar is designed to hold:
- Minimized dock icons (click to restore)
- Quick action buttons
- System tray integration
- Performance graphs

---

## Service Locator Pattern

### Purpose

Provides centralized access to core services (`MarketDataCore`, `DataCache`) without tight coupling.

**Location**: `libs/gui/widgets/ServiceLocator.hpp/.cpp`

### Implementation

```cpp
class ServiceLocator {
public:
    static void registerMarketDataCore(MarketDataCore* core);
    static void registerDataCache(DataCache* cache);
    static MarketDataCore* marketDataCore();
    static DataCache* dataCache();
private:
    static QPointer<MarketDataCore> s_marketDataCore;  // QPointer for QObject safety
    static DataCache* s_dataCache;  // Raw pointer (not QObject)
};
```

**Registration** (in `MainWindowGPU` constructor):
```cpp
ServiceLocator::registerMarketDataCore(m_marketDataCore.get());
ServiceLocator::registerDataCache(m_dataCache.get());
```

**Usage** (in any dock widget):
```cpp
auto* core = ServiceLocator::marketDataCore();
if (core) {
    core->subscribeToSymbols({symbol.toStdString()});
}
```

### Why QPointer?

- `MarketDataCore` inherits from `QObject`
- `QPointer` automatically becomes `nullptr` if object is deleted
- Prevents dangling pointer crashes
- `DataCache` is not a `QObject`, so uses raw pointer

### Future Evolution

Planned migration to signal-based service registry:
- Services emit `serviceAvailable()` signals
- Docks connect to these signals
- Enables hot-reload of modules
- Better for plugin architecture

---

## Signal/Slot Architecture

### Symbol Change Propagation

**Unified Signal**: `MainWindowGPU::symbolChanged(const QString& symbol)`

**Flow**:
```
User enters symbol → MainWindowGPU::onSubscribe()
    ↓
emit symbolChanged(symbol)
    ↓
All docks receive signal → DockablePanel::onSymbolChanged(symbol)
    ↓
Each dock updates its UI accordingly
```

**Implementation**:
```cpp
// In MainWindowGPU::setupUI()
connect(this, &MainWindowGPU::symbolChanged, m_secDock, &SecFilingDock::onSymbolChanged);
connect(this, &MainWindowGPU::symbolChanged, m_marketDataDock, &MarketDataPanel::onSymbolChanged);

// In MainWindowGPU::propagateSymbolChange()
emit symbolChanged(symbol);
for (auto* dock : findChildren<DockablePanel*>()) {
    dock->onSymbolChanged(symbol);  // Virtual method call
}
```

### Market Data Signals

**Thread Safety**: All market data signals use `Qt::QueuedConnection`

**Flow**:
```
MarketDataCore (worker thread)
    ↓ emit tradeReceived (queued)
UnifiedGridRenderer (GUI thread)
    ↓ process trade
DataProcessor (GUI thread)
    ↓ update visible cells
UnifiedGridRenderer::updatePaintNode()
    ↓ render to GPU
```

**Connection**:
```cpp
connect(m_marketDataCore.get(), &MarketDataCore::tradeReceived,
        unifiedGridRenderer, &UnifiedGridRenderer::onTradeReceived,
        Qt::QueuedConnection);  // Thread-safe queued connection
```

---

## Threading Model

### Thread Ownership

1. **Main GUI Thread**:
   - All Qt widgets (`QMainWindow`, `QDockWidget`, etc.)
   - `UnifiedGridRenderer`
   - `DataProcessor`
   - All UI updates

2. **Worker Thread**:
   - `MarketDataCore`
   - `BeastWsTransport` (WebSocket I/O)
   - `MessageDispatcher` (message parsing)

3. **Render Thread** (QML):
   - `QQuickView` rendering
   - GPU operations
   - Scene graph updates

### Cross-Thread Communication

**Pattern**: Always use `Qt::QueuedConnection` for cross-thread signals

**Example**:
```cpp
// Worker thread → GUI thread
connect(m_marketDataCore.get(), &MarketDataCore::connectionStatusChanged,
        this, &MainWindowGPU::onConnectionStatusChanged,
        Qt::QueuedConnection);
```

**Why Queued**: 
- Signals are posted to the receiving thread's event loop
- Slot executes on the correct thread
- No mutexes needed for simple data passing

### QML Thread Safety

**HeatmapDock Configuration**:
```cpp
m_qquickView->setPersistentSceneGraph(true);  // Keep scene graph alive
QSurfaceFormat format = QSurfaceFormat::defaultFormat();
m_qquickView->setFormat(format);
```

**Render Loop**: Set in `main.cpp`:
```cpp
qputenv("QSG_RENDER_LOOP", "threaded");  // Separate render thread
QQuickWindow::setGraphicsApi(QSGRendererInterface::GraphicsApi::OpenGLRhi);
```

This ensures QML rendering happens in a dedicated thread, preventing frame drops when docking events occur.

---

## Implementation Details

### MOC (Meta-Object Compiler) Requirements

Qt's MOC requires full type definitions for classes with `Q_OBJECT` macro.

**Solution**: Include widget headers directly in `MainWindowGpu.h`:
```cpp
// Forward declarations won't work for MOC
#include "widgets/HeatmapDock.hpp"
#include "widgets/StatusDock.hpp"
#include "widgets/StatusBar.hpp"
// ... etc
```

**Why**: MOC needs to see the full class definition to generate meta-object code for signals/slots.

### Virtual Destructors

**Rule**: All classes with `Q_OBJECT` that are base classes need virtual destructors.

**Example**:
```cpp
class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget* parent = nullptr);
    ~StatusBar() override = default;  // Virtual destructor required
};
```

**Why**: Prevents vtable errors during linking when MOC generates code.

### Layout Reset Mechanism

**Problem**: `QMainWindow::restoreState()` can't restore to a "default" state if no layout was saved.

**Solution**: Programmatic layout arrangement:
```cpp
void MainWindowGPU::resetLayoutToDefault() {
    sLog_App("Resetting layout to default");
    arrangeDefaultLayout();  // Programmatically arrange docks
    LayoutManager::deleteLayout(LayoutManager::defaultLayoutName());  // Clear saved state
}
```

**Flow**:
1. User presses Ctrl+R or selects "Reset to Default Layout"
2. `LayoutManager::resetToDefault()` called
3. Clears saved layout from `QSettings`
4. Invokes `MainWindowGPU::resetLayoutToDefault()` slot
5. `arrangeDefaultLayout()` removes all docks and re-adds them in default positions
6. Docks are shown and sized appropriately

### Performance Optimizations

1. **Repaint Coalescing**:
   ```cpp
   setUpdatesEnabled(false);  // Disable updates during layout changes
   // ... rearrange docks ...
   setUpdatesEnabled(true);   // Re-enable, single repaint
   ```

2. **Model-Based Views**: `MarketDataPanel` uses `QAbstractTableModel` instead of `QTableWidget` for better performance with streaming data.

3. **Update Throttling**: Status bar updates happen every 1 second (configurable via `QTimer`).

---

## Future Enhancements

### Planned Features

1. **Minimized Dock Icons**:
   - Docks can be minimized to status bar
   - Click icon to restore
   - Visual indicator when dock has updates

2. **Layout Presets**:
   - JSON-based layout descriptors
   - Version-controlled layouts
   - Easy sharing between users

3. **Command Palette**:
   - `Ctrl+P` to open searchable command list
   - Quick access to docks, layouts, actions
   - Similar to VSCode's command palette

4. **Plugin System**:
   - Auto-load docks from `plugins/` directory
   - `QPluginLoader` for dynamic loading
   - Hot-reload capability

5. **Service Registry Signals**:
   - Replace static `ServiceLocator` with signal-based system
   - Services emit availability signals
   - Better for plugin architecture

6. **Top Bar Menu Integration**:
   - Move View/Layout menus to top horizontal bar
   - Icon-based quick actions
   - More compact, modern look

### Extension Points

**Adding a New Dock**:

1. Create new class inheriting from `DockablePanel`:
   ```cpp
   class MyNewDock : public DockablePanel {
       Q_OBJECT
   public:
       explicit MyNewDock(QWidget* parent = nullptr);
       void buildUi() override;
       void onSymbolChanged(const QString& symbol) override;
   };
   ```

2. Add to `MainWindowGPU`:
   ```cpp
   // In MainWindowGpu.h
   MyNewDock* m_myNewDock = nullptr;
   
   // In MainWindowGpu.cpp::setupUI()
   m_myNewDock = new MyNewDock(this);
   addDockWidget(Qt::BottomDockWidgetArea, m_myNewDock);
   ```

3. Add to menu bar:
   ```cpp
   // In setupMenuBar()
   if (m_myNewDock) {
       viewMenu->addAction(m_myNewDock->toggleViewAction());
   }
   ```

4. Connect symbol changes:
   ```cpp
   connect(this, &MainWindowGPU::symbolChanged, m_myNewDock, &MyNewDock::onSymbolChanged);
   ```

**That's it!** The dock will automatically:
- Save/restore its position
- Respond to layout resets
- Appear in View menu
- Receive symbol change notifications

---

## Key Files Reference

### Core Framework
- `libs/gui/MainWindowGpu.h/.cpp` - Main window and dock management
- `libs/gui/widgets/DockablePanel.hpp/.cpp` - Base class for all docks
- `libs/gui/widgets/LayoutManager.hpp/.cpp` - Layout save/restore
- `libs/gui/widgets/ServiceLocator.hpp/.cpp` - Service access

### Dock Widgets
- `libs/gui/widgets/HeatmapDock.hpp/.cpp` - GPU heatmap with embedded controls
- `libs/gui/widgets/MarketDataPanel.hpp/.cpp` - Market data table
- `libs/gui/widgets/SecFilingDock.hpp/.cpp` - SEC filing viewer
- `libs/gui/widgets/CopenetFeedDock.hpp/.cpp` - COPENET commentary
- `libs/gui/widgets/AICommentaryFeedDock.hpp/.cpp` - AI commentary
- `libs/gui/widgets/StatusDock.hpp/.cpp` - Central widget placeholder
- `libs/gui/widgets/StatusBar.hpp/.cpp` - Bottom status bar

### Build Configuration
- `libs/gui/CMakeLists.txt` - Uses `file(GLOB WIDGET_SOURCES "widgets/*.cpp")` for automatic widget discovery

---

## Troubleshooting

### Layout Won't Reset

**Symptom**: Pressing Ctrl+R doesn't restore default layout.

**Solution**: 
1. Check `resetLayoutToDefault()` is declared as a slot in `MainWindowGpu.h`
2. Verify `LayoutManager::resetToDefault()` uses `QMetaObject::invokeMethod()`
3. Ensure `arrangeDefaultLayout()` properly removes and re-adds docks

### Docks Don't Save Position

**Symptom**: Dock positions reset on app restart.

**Solution**:
1. Verify `closeEvent()` calls `LayoutManager::saveLayout()`
2. Check `QSettings` path is writable
3. Ensure dock `setObjectName()` matches saved layout

### MOC Errors

**Symptom**: Compilation errors about incomplete types.

**Solution**:
1. Include full widget headers in `MainWindowGpu.h` (not just forward declarations)
2. Ensure all `Q_OBJECT` classes have virtual destructors
3. Run `cmake --preset <preset>` to regenerate MOC files

### Status Bar Not Updating

**Symptom**: Connection status doesn't update in bottom bar.

**Solution**:
1. Verify `onConnectionStatusChanged()` calls `m_statusBar->setConnectionStatus()`
2. Check signal connection uses `Qt::QueuedConnection`
3. Ensure `m_statusBar` is not null

---

## Conclusion

The Sentinel dockable framework provides a robust, extensible foundation for building a professional trading terminal interface. By leveraging Qt's docking system and adding our own abstractions, we've created a system that is:

- **Modular**: Easy to add new docks
- **Persistent**: Layouts save/restore automatically
- **Thread-Safe**: Proper cross-thread communication
- **Performant**: Optimized rendering and updates
- **Extensible**: Clear extension points for future features

The architecture balances flexibility with simplicity, making it easy to add new features while maintaining code quality and performance.

