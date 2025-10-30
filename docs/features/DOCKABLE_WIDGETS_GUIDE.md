# Dockable Widget Architecture Guide

This document outlines how to extend the existing Sentinel Qt front-end so that heatmap panels and
new SEC tooling can be repositioned, tabbed, or floated like a desktop trading terminal. The guide
assumes familiarity with the current QWidget-based shell implemented in
`libs/gui/MainWindowGpu.cpp`.

## 1. Objectives

1. Replace the fixed layout in `MainWindowGPU` with a dockable workspace while preserving the QML
   heatmap rendered through `QQuickView`.
2. Provide a reusable dockable widget abstraction so non-QML tools (e.g., SEC data panels) can be
   added without rewriting layout code.
3. Define how the existing Python SEC stack (see the `sec/` package) should be exposed to Qt widgets
   so the fetcher UI can be reimplemented with native controls.

## 2. Current State Recap

* `MainWindowGPU` subclasses `QWidget`, builds a `QVBoxLayout`, and embeds the heatmap with
  `QQuickView` via `QWidget::createWindowContainer` alongside a control panel widget.
  The relevant setup code lives in `MainWindowGPU::setupUI()`.
* The QML scene (`libs/gui/qml/DepthChartView.qml`) exposes the heatmap renderer object named
  `unifiedGridRenderer`. The root context already receives the active symbol and a
  `ChartModeController` pointer.
* SEC functionality lives in Python under `sec/`. `SECDataFetcher` encapsulates all EDGAR calls and
  caching, while the DearPyGui widget implementation exists in `sec/sec_filing_viewer.py`.

The dockable redesign keeps the rendering pipeline unchanged while reorganizing how widgets are
composed.

## 3. Upgrade `MainWindowGPU` to `QMainWindow`

1. Change the inheritance to `class MainWindowGPU : public QMainWindow` and include `<QMainWindow>`.
2. Move the existing layout-created widgets into explicit child widgets:
   * Create a `QWidget` (e.g., `auto centralPanel = new QWidget;`) with the former `QVBoxLayout` and
     call `setCentralWidget(centralPanel);`.
   * The heatmap container returned by `QWidget::createWindowContainer` remains the primary child of
     the layout, preserving `QQuickView` setup.
   * Wrap the bottom "Trading Controls" section in its own widget (e.g., `QWidget* controlsBar`).
3. Enable docking behavior by calling `setDockOptions(QMainWindow::AllowTabbedDocks |
   QMainWindow::AnimatedDocks);` and `setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);` in
   the constructor after `setupUI()`.

This conversion maintains the existing lifecycle logic in `initializeQMLComponents()` and
`connectMarketDataSignals()` while unlocking Qt's native dock manager.

## 4. Heatmap as a Dockable Widget

1. Promote the heatmap container to a `QDockWidget`:
   * Instantiate `auto heatmapDock = new QDockWidget(tr("Heatmap"), this);`.
   * Set `heatmapDock->setObjectName("HeatmapDock");` so layout state can be restored.
   * Call `heatmapDock->setWidget(m_qmlContainer);` after the `QQuickView` is configured.
2. Use `setCentralWidget` for a lightweight placeholder such as a status panel, or leave the central
   area empty with `auto spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding,
   QSizePolicy::Expanding); setCentralWidget(spacer);`.
3. Add the dock to the default location with `addDockWidget(Qt::RightDockWidgetArea, heatmapDock);`
   and enable floating via `heatmapDock->setFeatures(QDockWidget::AllDockWidgetFeatures);`.
4. Persist layouts by saving/restoring state: in the constructor instantiate `QSettings settings(
   "MyCompany", "Sentinel");` and call `restoreState(settings.value("windowState").toByteArray());`.
   Override `closeEvent()` to create a fresh `QSettings` and invoke
   `settings.setValue("windowState", saveState());` before delegating to `QMainWindow::closeEvent()`.
   Using `QSettings` in this way keeps the layout across sessions by reading the stored
   `windowState` on start and writing it back when closing.

Because the heatmap is still driven by `QQuickView`, no changes are required in QML or rendering
logic.

## 5. Dockable Widget Helper Abstraction

To simplify future panes:

1. Introduce a small helper class, e.g., `class DockablePanel : public QDockWidget`, in
   `libs/gui/widgets/` that sets standard flags, stores a persistent identifier, and exposes a
   virtual `buildUi()` method.
2. For widgets that need access to shared services (market data, monitors, etc.), pass pointers from
   `MainWindowGPU` through the constructor or register them in a lightweight service locator.
3. Offer factory helpers in `MainWindowGPU` such as `createDock(const QString& id, QWidget* content,
   Qt::DockWidgetArea area)` to standardize naming, default positions, and tab behavior.

Heatmap, order-book, news, and SEC panels can all inherit from this helper to gain consistent menu
entries for show/hide and serialization hooks.

## 6. Menu Bar and Tab Management

1. After switching to `QMainWindow`, call `menuBar()` to add standard menus:
   * **View** menu toggles dock visibility via the `toggleViewAction()` from each dock widget.
   * **Layouts** menu stores multiple layouts by capturing `saveState()` blobs to disk.
   * **Tools** menu exposes actions like "Open SEC Filing Viewer".
2. Enable tabbing via `setDockOptions(AllowTabbedDocks)` and call
   `tabifyDockWidget(firstDock, secondDock)` to group related panels (e.g., Heatmap + Time & Sales).
3. Provide shortcuts (`QShortcut`) for quick layout switching and docking commands.

## 7. Porting the SEC Filing Viewer

### 7.1 Service Layer Bridging

1. Reuse the Python `SECDataFetcher` (`sec/sec_api.py`) by exposing an async microservice started
   alongside the Qt app (e.g., FastAPI or simple `asyncio` server) or by embedding Python with
   `PyBind11` if a direct in-process call is desired.
2. Start with a separate process to reduce risk: spawn the Python service via `QProcess` inside
   `MainWindowGPU` and communicate over HTTP/WebSockets. This mirrors the async patterns already
   used by `SECDataFetcher` and keeps rate-limiting logic intact.
3. Define JSON contracts that mirror the DearPyGui widget expectations (filings list, insider
   transactions, financial summaries). The service can return the same payload dictionaries already
   produced by `SECFilingViewer` callback helpers.

### 7.2 Qt Dock Implementation

1. Implement `SecFilingDock` (inherits from the dock helper) in `libs/gui/widgets/`.
   * Build the UI with standard Qt widgets (`QLineEdit` for ticker, `QComboBox` for form type,
     `QTableView` bound to `QStandardItemModel` for filings/transactions, `QTextEdit` for financial
     summaries).
   * Use `QNetworkAccessManager` to call the Python service asynchronously; parse JSON into models.
   * Provide loading indicators and error banners similar to the `_update_status` logic in the
     DearPyGui version.
2. Register the dock from `MainWindowGPU` (`createSecDock()` helper) and add it to a default area,
   tabbed with the heatmap so users can toggle between market view and SEC data quickly.
3. Hook menu actions and keyboard shortcuts to show/hide the SEC dock, mirroring the behavior of the
   DearPyGui dock.

### 7.3 Event Wiring

1. Connect the heatmap symbol changes to the SEC dock: when `MainWindowGPU::onSubscribe()` sets the
   new symbol context, emit a Qt signal (`symbolChanged(const QString&)`). The SEC dock listens and
   optionally auto-loads filings for the active instrument.
2. Support manual refresh: the SEC dock should expose actions analogous to
   `_fetch_filings_callback`, `_fetch_insider_tx_callback`, and `_fetch_financials_callback` from the
   Python widget.
3. Cache responses locally in the Qt layer if the user undocks the panel to avoid re-fetching when
   it is reattached.

## 8. Testing and Validation

1. Smoke test by verifying the heatmap still renders and updates after docking/undocking and across
   layout save/restore cycles.
2. Confirm the SEC dock handles network failures gracefully by simulating timeouts from the Python
   service.
3. Ensure all dock widgets respond to `toggleViewAction()` and that tab reordering persists between
   sessions via `saveState()` snapshots.

Following these steps will produce a modular, dock-friendly Sentinel UI while reusing the existing
rendering stack and SEC data orchestration code.
