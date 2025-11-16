# Widget Communication Architecture

## Overview

Sentinel uses a **hub-and-spoke** communication model where `MainWindowGPU` acts as the central coordinator. Widgets communicate through:

1. **Qt Signals/Slots** - For event-driven communication
2. **ServiceLocator** - For shared service access (singleton pattern)
3. **Virtual method hooks** - For direct method calls

---

## Current Widgets

### Active Widgets

| Widget | Purpose | Inherits From |
|--------|---------|---------------|
| `HeatmapDock` | GPU-accelerated trading heatmap + embedded symbol controls | `DockablePanel` |
| `StatusDock` | Central placeholder widget (minimal) | `QWidget` |
| `StatusBar` | Bottom status bar (CPU/GPU/Latency metrics) | `QWidget` |
| `MarketDataPanel` | Real-time trade data table | `DockablePanel` |
| `SecFilingDock` | SEC filings viewer | `DockablePanel` |
| `CopenetFeedDock` | Copenet commentary feed | `CommentaryFeedDock` |
| `AICommentaryFeedDock` | AI commentary feed | `CommentaryFeedDock` |

### Base Classes

- **`DockablePanel`** - Base class for all dockable widgets
  - Provides `onSymbolChanged()` virtual hook
  - Handles common dock widget setup
  - All docks inherit from this

- **`CommentaryFeedDock`** - Base for feed widgets (`CopenetFeedDock`, `AICommentaryFeedDock`)
  - Provides message appending/pruning logic
  - Shared functionality for feed displays

---

## Communication Patterns

### 1. Symbol Change Propagation (Pub/Sub)

**Publisher:** `MainWindowGPU`  
**Subscribers:** All `DockablePanel` widgets

```cpp
// MainWindowGPU emits signal
emit symbolChanged("BTC-USD");

// Two propagation methods:
// 1. Qt Signal/Slot (for widgets that connect explicitly)
connect(this, &MainWindowGPU::symbolChanged, m_secDock, &SecFilingDock::onSymbolChanged);

// 2. Virtual method call (for all DockablePanel children)
for (auto* dock : findChildren<DockablePanel*>()) {
    dock->onSymbolChanged(symbol);
}
```

**How to subscribe:**
```cpp
// In your widget's constructor or setupConnections():
connect(mainWindow, &MainWindowGPU::symbolChanged, this, &MyWidget::onSymbolChanged);

// Or override the virtual method:
void MyWidget::onSymbolChanged(const QString& symbol) override {
    // React to symbol change
}
```

### 2. Shared Services (ServiceLocator)

**Access shared services** without direct parent references:

```cpp
// Get MarketDataCore (for subscribing to symbols)
MarketDataCore* core = ServiceLocator::marketDataCore();
if (core) {
    core->subscribeToSymbols({"BTC-USD"});
}

// Get DataCache (for reading cached trades/orderbooks)
DataCache* cache = ServiceLocator::dataCache();
if (cache) {
    auto trades = cache->getRecentTrades("BTC-USD", 100);
}
```

**Services registered in `MainWindowGPU` constructor:**
```cpp
ServiceLocator::registerMarketDataCore(m_marketDataCore.get());
ServiceLocator::registerDataCache(m_dataCache.get());
```

### 3. Direct Signal Connections

**Connect to MarketDataCore signals** for real-time data:

```cpp
// In your widget's setupConnections():
connect(ServiceLocator::marketDataCore(), &MarketDataCore::tradeReceived,
        this, &MyWidget::onTradeReceived, Qt::QueuedConnection);

connect(ServiceLocator::marketDataCore(), &MarketDataCore::liveOrderBookUpdated,
        this, &MyWidget::onOrderBookUpdated, Qt::QueuedConnection);
```

**Important:** Always use `Qt::QueuedConnection` when connecting from worker threads to GUI thread!

---

## Creating a New Widget

### Step 1: Create the Widget Class

```cpp
// MyNewWidget.hpp
#pragma once
#include "DockablePanel.hpp"

class MyNewWidget : public DockablePanel {
    Q_OBJECT
public:
    explicit MyNewWidget(QWidget* parent = nullptr);
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;  // Optional
    
private slots:
    void onSomeEvent();
    
private:
    // Your widgets here
};
```

### Step 2: Implement buildUi()

```cpp
// MyNewWidget.cpp
#include "MyNewWidget.hpp"
#include "ServiceLocator.hpp"

MyNewWidget::MyNewWidget(QWidget* parent)
    : DockablePanel("MyNewWidget", "My Widget", parent)
{
    buildUi();
}

void MyNewWidget::buildUi() {
    // Create your UI here
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    // ... add widgets ...
}

void MyNewWidget::onSymbolChanged(const QString& symbol) {
    // React to symbol changes
}
```

### Step 3: Register in MainWindowGPU

**In `MainWindowGpu.h`:**
```cpp
#include "widgets/MyNewWidget.hpp"
// ...
MyNewWidget* m_myNewWidget = nullptr;
```

**In `MainWindowGpu.cpp` `setupUI()`:**
```cpp
m_myNewWidget = new MyNewWidget(this);
addDockWidget(Qt::RightDockWidgetArea, m_myNewWidget);
```

**In `setupMenuBar()`:**
```cpp
if (m_myNewWidget) {
    viewMenu->addAction(m_myNewWidget->toggleViewAction());
}
```

**In `arrangeDefaultLayout()`:**
```cpp
addDockWidget(Qt::BottomDockWidgetArea, m_myNewWidget);
m_myNewWidget->setVisible(true);
```

---

## Pub/Sub Examples

### Publishing a Custom Event

**Option 1: Add signal to MainWindowGPU**
```cpp
// MainWindowGpu.h
signals:
    void myCustomEvent(const QString& data);

// Emit it:
emit myCustomEvent("some data");

// Widgets connect:
connect(mainWindow, &MainWindowGPU::myCustomEvent, this, &MyWidget::onCustomEvent);
```

**Option 2: Use ServiceLocator with signals** (future enhancement)
```cpp
// If MarketDataCore emits a signal, widgets can connect directly:
connect(ServiceLocator::marketDataCore(), &MarketDataCore::someSignal,
        this, &MyWidget::onSomeSignal, Qt::QueuedConnection);
```

### Subscribing to Events

```cpp
// In widget constructor or setupConnections():
void MyWidget::setupConnections() {
    // Subscribe to symbol changes
    connect(mainWindow, &MainWindowGPU::symbolChanged, 
            this, &MyWidget::onSymbolChanged);
    
    // Subscribe to market data
    auto* core = ServiceLocator::marketDataCore();
    if (core) {
        connect(core, &MarketDataCore::tradeReceived,
                this, &MyWidget::onTradeReceived, Qt::QueuedConnection);
    }
}
```

---

## Thread Safety

- **GUI Thread:** All widgets run on the GUI thread
- **Worker Threads:** `MarketDataCore` runs on worker threads
- **Connection Type:** Always use `Qt::QueuedConnection` when connecting worker thread signals to GUI slots
- **ServiceLocator:** Safe to call from any thread (but GUI updates must happen on GUI thread)

---

## Summary

**Widget Communication Flow:**

```
MainWindowGPU (Hub)
    ├── Emits: symbolChanged() signal
    ├── Calls: dock->onSymbolChanged() virtual method
    └── Registers: Services via ServiceLocator
    
Widgets (Spokes)
    ├── Subscribe: connect() to MainWindowGPU signals
    ├── Override: onSymbolChanged() virtual method
    └── Access: ServiceLocator::marketDataCore() / dataCache()
```

**Key Principles:**
1. **Centralized coordination** via `MainWindowGPU`
2. **Loose coupling** via signals/slots and ServiceLocator
3. **Consistent interface** via `DockablePanel` base class
4. **Thread-safe** via `Qt::QueuedConnection` for cross-thread communication




