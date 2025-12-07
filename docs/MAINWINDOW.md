# Architecture

### Component Structure

```
libs/gui/mainwindow/
├── DataBootstrapper.{h,cpp}      # Config loading & data component initialization
├── DockFactory.{h,cpp}            # Dock widget creation
├── QmlSceneController.{h,cpp}    # QML loading, GPU verification, context properties
├── LayoutOrchestrator.{h,cpp}    # Layout arrangement & persistence
├── MenuBuilder.{h,cpp}            # Menu bar construction
└── ShortcutBinder.{h,cpp}         # Keyboard shortcut registration
```

### Component Responsibilities

#### DataBootstrapper

**Purpose**: Isolates configuration loading and data component initialization.

**Key Features**:
- Loads `QSettings` to extract authentication key file path
- Creates `Authenticator`, `DataCache`, and `MarketDataCore` instances
- Starts `MarketDataCore` worker threads
- Returns `DataComponents` struct for ownership transfer

**Usage**:
```cpp
auto dataComponents = DataBootstrapper::initialize();
m_marketDataCore = std::move(dataComponents.marketDataCore);
// ... register with ServiceLocator
```

**Benefits**:
- No Qt dependencies beyond `QSettings` (keeps core clean)
- Testable independently
- Single point for config changes

#### DockFactory

**Purpose**: Centralizes dock widget creation and initial sizing.

**Key Features**:
- Creates all dock widgets (`HeatmapDock`, `MarketDataPanel`, `SecFilingDock`, etc.)
- Sets initial minimum sizes for proportional layout
- Returns dock references and symbol controls via structs

**Usage**:
```cpp
DockFactory dockFactory(this);
auto docks = dockFactory.createDocks();
auto symbolControls = dockFactory.getSymbolControls();
```

**Benefits**:
- Dock creation logic in one place
- Easy to add new docks without touching `MainWindowGPU`
- Consistent sizing initialization

#### QmlSceneController

**Purpose**: Manages QML scene lifecycle, GPU verification, and context properties.

**Key Features**:
- Loads QML source (config/env aware)
- Verifies GPU acceleration and logs API (OpenGL/Direct3D11/Vulkan/Metal)
- Sets QML context properties (symbol, `ChartModeController`)
- Provides `UnifiedGridRenderer*` accessor

**Usage**:
```cpp
m_qmlController = std::make_unique<QmlSceneController>(m_qquickView);
m_qmlController->loadQmlSource();
m_qmlController->verifyGpuAcceleration();
m_qmlController->setChartModeController(modeController);
```

**Benefits**:
- QML loading logic isolated
- GPU verification centralized
- Easy to swap QML sources for testing

#### LayoutOrchestrator

**Purpose**: Handles dock arrangement, sizing, and layout persistence.

**Key Features**:
- Arranges docks in default layout (left heatmap, right sidebar, bottom feeds)
- Screen-percentage-based sizing (70% heatmap, 30% sidebar, 90% main height, 10% bottom)
- Integrates with `LayoutManager` for save/restore
- Handles dock tabbing and nested layouts

**Layout Proportions**:
- **Horizontal**: 70% heatmap (left), 30% sidebar (right)
- **Vertical**: 90% main area height, 10% bottom commentary feeds
- **Right sidebar**: Tabbed SEC Filing Viewer and Market Data Panel
- **Bottom**: Tabbed COPENET and AI Commentary feeds

**Usage**:
```cpp
m_layoutOrchestrator = std::make_unique<LayoutOrchestrator>(this);
if (!m_layoutOrchestrator->restoreLayout(defaultLayoutName())) {
    m_layoutOrchestrator->arrangeDefaultLayout(docks);
}
```

**Benefits**:
- Layout math isolated from window logic
- Easy to adjust proportions
- Consistent layout restoration

#### MenuBuilder

**Purpose**: Constructs menu bar with View, Layouts, and Tools menus.

**Key Features**:
- Builds menus from dock widgets and callbacks
- Keeps dialog prompts out of `MainWindowGPU`
- Uses callback pattern for actions (no direct coupling)

**Usage**:
```cpp
MenuBuilder::Callbacks callbacks;
callbacks.saveLayout = [this]() { onSaveLayout(); };
callbacks.restoreLayout = [this]() { onRestoreLayout(); };
// ... set other callbacks
m_menuBuilder->buildMenus(docks, callbacks);
```

**Benefits**:
- Menu logic separated from window
- Easy to add new menu items
- Testable via callback injection

#### ShortcutBinder

**Purpose**: Registers keyboard shortcuts and binds them to callbacks.

**Key Features**:
- Registers shortcuts (Ctrl+S/L/R, F1-F3, F11)
- Binds to callbacks for layout operations and dock toggles
- Handles fullscreen toggle

**Usage**:
```cpp
ShortcutBinder::Callbacks callbacks;
callbacks.saveLayout = [this]() { onSaveLayout(); };
m_shortcutBinder->bindShortcuts(callbacks, docks);
```

**Benefits**:
- Shortcut registration centralized
- Easy to add new shortcuts
- No direct coupling to window methods

## Refactored MainWindowGPU

### Before

```cpp
MainWindowGPU::MainWindowGPU(QWidget* parent) {
    initializeDataComponents();      // 50+ lines inline
    setupUI();                        // 150+ lines inline
    setupMenuBar();                   // 80+ lines inline
    setupShortcuts();                 // 60+ lines inline
    setupConnections();
    // ... more setup
}
```

### After

```cpp
MainWindowGPU::MainWindowGPU(QWidget* parent) {
    // 1) Initialize data components
    auto dataComponents = DataBootstrapper::initialize();
    m_marketDataCore = std::move(dataComponents.marketDataCore);
    // ... register services
    
    // 2) Create dock widgets
    setupUI();  // Now just calls DockFactory
    
    // 3) Initialize QML scene
    m_qmlController = std::make_unique<QmlSceneController>(m_qquickView);
    m_qmlController->loadQmlSource();
    
    // 4) Set up layout
    m_layoutOrchestrator = std::make_unique<LayoutOrchestrator>(this);
    // ... restore or arrange default
    
    // 5) Set up menus and shortcuts
    m_menuBuilder = std::make_unique<MenuBuilder>(menuBar());
    m_shortcutBinder = std::make_unique<ShortcutBinder>(this);
    setupMenuBar();  // Now just wires callbacks
    setupShortcuts(); // Now just wires callbacks
}
```

### Key Improvements

1. **Clear initialization order**: Each step is explicit and numbered
2. **Reduced complexity**: `MainWindowGPU` constructor is ~40 lines vs 100+
3. **Better separation**: Each component owns its domain
4. **Easier testing**: Components can be tested independently
5. **Easier extension**: Add new docks/menus/shortcuts without touching core

## Integration Points

### ServiceLocator Registration

After `DataBootstrapper` creates components, `MainWindowGPU` registers them:

```cpp
ServiceLocator::registerMarketDataCore(m_marketDataCore.get());
ServiceLocator::registerDataCache(m_dataCache.get());
```

### Signal/Slot Connections

`MainWindowGPU` still handles high-level signal connections:

```cpp
connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookUpdated,
        dataProcessor, &DataProcessor::onLiveOrderBookUpdated, Qt::QueuedConnection);
```

Component-specific connections (menu actions, shortcuts) are handled via callbacks.

## Threading Model

All components run on the **main GUI thread**. This is intentional:
- **Setup-only**: Components are created during window construction
- **Not on hot path**: No performance impact on data rendering
- **Qt requirements**: Menu/shortcut/dock operations must be on GUI thread

`MarketDataCore` (created by `DataBootstrapper`) spawns worker threads internally, but component initialization is synchronous.

## Testing Strategy

Each component can be tested independently:

- **DataBootstrapper**: Mock `QSettings`, verify component creation
- **DockFactory**: Verify dock creation and initial sizes
- **QmlSceneController**: Mock `QQuickView`, verify QML loading and context properties
- **LayoutOrchestrator**: Mock `QMainWindow`, verify dock arrangement
- **MenuBuilder/ShortcutBinder**: Verify menu/shortcut creation with callbacks

## Future Extensions

### Adding a New Dock

1. Create dock widget inheriting from `DockablePanel`
2. Add to `DockFactory::createDocks()`
3. Add to `LayoutOrchestrator::DockWidgets` struct
4. Add to `MenuBuilder::DockWidgets` for View menu
5. Wire symbol changes in `MainWindowGPU::setupUI()`

### Adding a New Menu Item

1. Add callback to `MenuBuilder::Callbacks`
2. Implement callback method in `MainWindowGPU`
3. Wire callback in `MainWindowGPU::setupMenuBar()`
4. Add menu item in `MenuBuilder::buildMenus()`

### Adding a New Shortcut

1. Add callback to `ShortcutBinder::Callbacks` (if layout-related)
2. Implement callback method in `MainWindowGPU`
3. Wire callback in `MainWindowGPU::setupShortcuts()`
4. Register shortcut in `ShortcutBinder::bindShortcuts()`

## Migration Notes

- **No API changes**: External code using `MainWindowGPU` is unaffected
- **Behavior preserved**: All existing functionality maintained
- **Layout proportions**: Updated to 90% heatmap height, 10% bottom feeds
- **File locations**: Components in `libs/gui/mainwindow/`, `MainWindowGPU` remains at `libs/gui/`

## Related Documentation

- `docs/ARCHITECTURE.md`: High-level architecture overview
- `docs/features/aesthetic/DOCKABLE_FRAMEWORK.md`: Dock widget system details
- `docs/features/aesthetic/WIDGET_COMMUNICATION.md`: Signal/slot patterns