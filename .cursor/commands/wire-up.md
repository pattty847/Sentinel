# Wire Up New Component

Help connect a new component to Sentinel's existing architecture.

**Common scenarios:**
- New dock widget (HeatmapDock, OrderBookDock pattern)
- New data source (IGridDataSource implementation)
- New rendering component (QSG node, renderer)
- New market data handler (MessageDispatcher extension)

**Process:**
1. **Identify the component type:**
   - Dock widget → DockablePanel pattern
   - Data source → IGridDataSource interface
   - Renderer → UnifiedGridRenderer integration
   - Market data → MessageDispatcher/MarketDataCoreEngine

2. **Find integration points:**
   - Where does it register? (MainWindowGPU constructor, ServiceLocator)
   - What signals/slots connect it? (hub-and-spoke pattern)
   - What services does it need? (MarketDataCoreQt, IGridDataSource)
   - What thread does it run on? (main thread for GUI, worker for data)

3. **Generate integration code:**
   - Registration code (MainWindowGPU, menu, layout)
   - Signal/slot connections (Qt::QueuedConnection for cross-thread)
   - Service dependencies (ServiceLocator pattern)
   - Threading setup (if needed)

**Output format:**
- **Integration points:** Files to modify, functions to call
- **Code snippets:** Ready-to-use registration/connection code
- **Dependencies:** What needs to be added to CMakeLists.txt
- **Threading notes:** Which thread, how to communicate
- **Testing checklist:** How to verify it works

**Rules:**
- Follow Sentinel's patterns exactly (DockablePanel, hub-and-spoke, ServiceLocator)
- Respect threading rules (main thread for GUI, worker for data)
- Use Qt::QueuedConnection for cross-thread signals
- No direct widget-to-widget calls
- Core stays pure C++, GUI owns Qt

**Reference:**
- AGENTS.md section 5 (Dockable Framework)
- Existing docks for examples (HeatmapDock, SecFilingDock)

Be specific with file paths and function names. Don't give vague "update the handler" instructions.
