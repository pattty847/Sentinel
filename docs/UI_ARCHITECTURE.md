# Sentinel GUI Architecture (libs/gui)

## 1. Overview
The `libs/gui` module is strictly responsible for declarative UI management (QML/Widgets), dock layout orchestration, and high-performance GPU-resident rendering. It guarantees strict separation from Core business logic, adhering to the client-server design where the GUI acts solely as a consumer of `IGridDataSource`.

## 2. Core Operational Components

### MainWindowGpu
`MainWindowGpu` is the central host for the GUI process. It lives exclusively on the main thread and has the following responsibilities:
*   **Layout & Dock Management:** Manages dockable widgets (HeatmapDock, SecFilingDock, ScreenerDock, etc.) via `LayoutOrchestrator` and `DockFactory`.
*   **Scene Control:** Initializes and bridges the declarative QML environment (`QmlSceneController`) to the underlying C++ backend.
*   **Connection Lifecycle:** Establishes connection to the remote backend (`RemoteGridDataSource`) and routes connection status changes to the UI layer.
*   **State Propagation:** Distributes state updates (e.g., Active Symbol) down to independent docks and controllers via signals.

### DataProcessor
The `DataProcessor` bridges the gap between the `IGridDataSource` networking layer and the GPU render engine.
*   **Background Processing:** Operates on its own thread (`m_dataProcessorThread`) to prevent any deserialization or column-building overhead from blocking the main GUI event loop.
*   **Slice Ingestion:** Ingests raw structured slices (`HeatmapSlice`, `FootprintSlice`, `TpoSlice`, `VolumeProfileSlice`).
*   **Emission:** Emits parsed, render-ready byte columns and states (e.g., `heatmapColumnReady`, `tpoColumnReady`) back to the main thread via `Qt::QueuedConnection`.

## 3. The Rendering Pipeline

The rendering pipeline is coordinated by `UnifiedGridRenderer`, which inherits from `QQuickItem`. It acts as the singular canvas onto which all market data visual strategies are explicitly painted.

### UnifiedGridRenderer (UGR)
UGR implements the `QSGNode` lifecycle via `updatePaintNode()`. 
*   **FrameContext Construction:** At the start of every render pass, UGR constructs an immutable `FrameContext`, which contains the current viewport, time mapping, and stream generations.
*   **Thread Safety:** Validates incoming state via thread-safe mutexes (e.g., `m_vpMutex` for Volume Profile, `m_footprintPendingMutex` for footprint textures). The render thread never touches full QObject graphs.
*   **Orchestration:** Hands off the `FrameContext` to the various render strategies (overlays).

### Render Strategies (Overlays)
Overlays are independent C++ classes that manage their respective GPU resources (textures, materials, geometry) and write onto the UGR canvas.
*   **HeatmapOverlayRenderer:** Uploads rank-indexed byte columns into highly volatile QSG textures, mapped via custom shaders.
*   **FootprintOverlayRenderer:** Similar ring-buffer texture upload, visually distinct from heatmap, overlaying text-based order flow geometry.
*   **TpoOverlayRenderer:** Supports two Display Modes (`HorizontalProfile` and `VerticalTimeline`). It projects letter distributions onto the GPU.
*   **VolumeProfileRenderer:** Driven by `VolumeProfileState`, visualizing price-domain histograms on the horizontal bounds.

## 4. Coordinate Systems and Mapping

### CoordinateSystem
A stateless, QML-compatible math utility class that performs simple arithmetic transformations between World (price/time) and Screen (pixels) given a specific `Viewport`. 

### TimeAxisMapping
`TimeAxisMapping` is the single source of truth produced by UGR on every frame.
*   It guarantees that all overlays map coordinates consistently. If the viewport pans or zooms, the `TimeAxisMapping` is rebuilt in `updatePaintNode()` and propagated via the `FrameContext`. 
*   **Domains:** Maintains strict boundaries between World, Grid (texture col/row), and Screen, ensuring `timeOffset` remains an isolated shader parameter for ring textures rather than leaking into candlestick or label Math.