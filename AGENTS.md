## **AGENTS.md — Sentinel Unified Assistant Prompt (v5.2)**

**Source of truth for all AI assistants working on Sentinel.**
Claude, Gemini, Cursor, ChatGPT must follow this file.

---

# **1. Mission**

Sentinel is a GPU-accelerated trading terminal.
Its identity rests on three pillars:

* **Core stays pure C++ (no Qt contamination).**
* **GUI owns all Qt/QML/QSG behavior.**
* **Rendering is GPU-first, zero-lag, and deterministic.**

Everything the AI does must protect those three truths.

---

# **2. Architecture Overview (Simple & Honest)**

### **Core Layer (`libs/core`)**

* No Qt except QString/QDateTime if needed.
* Handles: market data transport, dispatchers, order books, protocol DTOs, transforms.

### **GUI Layer (`libs/gui`)**

* Owns Qt, QML, QSG, rendering strategies, widget layouts.
* Contains the docking system & communication between widgets.

### **Apps (`apps/`)**

* Thin bootstraps. No business logic.

### **Render Path**

```
Server: Transport → Dispatch → MarketDataCoreEngine → ServerDataModel → HeatmapTwapStreamer
        → SentinelStreamServer → WebSocket
Client: SentinelStreamClient → RemoteGridDataSource → DataProcessor → UnifiedGridRenderer
        → HeatmapIntensityNode + HeatmapGlyphNode → GPU
```

### **Invariant**

**Zoom/pan must always update via `setViewport()` so viewportVersion increments.**
Never mutate viewport fields directly.
If viewportVersion doesn’t change, the grid won’t rebuild.

---

# **3. Coding Standards (Realistic Solo-Dev Edition)**

### **Musts**

* Use modern C++20: RAII, smart pointers, no naked new/delete.
* Prefer explicit types for public APIs.
* Use expressive names; comment only where intent is subtle.
* Separate concerns: logic in core, visuals in gui.
* If you add an ENV VAR to the program, make sure to outline it to docs/ENV_VARS.md.
* Qt resources: when adding SVGs/icons via CMake, use `QT_RESOURCE_ALIAS` (or explicit aliases) so runtime paths are stable (e.g., `:/icons/icon-*.svg`). Avoid absolute path aliases.

### **Threading Rules**

* Network & data processing off GUI thread.
* Cross-thread communication only via Qt::QueuedConnection.
* QSG strategies never touch QObject graph.

### **File Size**

* No arbitrary LOC limit.
* If file feels unwieldy, split when *you* feel it’s time.

---

# **4. Branch Workflow (Non-Bureaucratic)**

### **Branches**

* `main` — stable, demo-ready.
* `dev` — messy high-velocity work.
* `feature/<name>` — only for large refactors.

### **Rules**

* Rebase feature onto dev frequently.
* Don’t stack branches.
* Commit as often as you want; clean history optional.

If you can understand your commit messages tomorrow morning, they’re good.

**GUI Screenshot API**  
Exposes a local HTTP endpoint for automated screenshots of the UI.

- **Endpoint:** `GET http://127.0.0.1:<PORT>/screenshot`
- **Params:**
    - `target`: What to capture—`main` (default), `heatmap`, or `lab`
    - `name`: (Optional) Filename override
- **Env Vars:**
    - `SENTINEL_GUI_API_PORT` (default: 17100)
    - `SENTINEL_GUI_SCREENSHOT_DIR` (default: `./screenshots`)
- **Examples:**
    - Full window:  
      `curl "http://127.0.0.1:17100/screenshot"`
    - Heatmap only:  
      `curl "http://127.0.0.1:17100/screenshot?target=heatmap"`
- **Flow:**  
  Run app → modify UI → call screenshot API → PNG saved → review.

---

# **5. Dockable Framework**

### **Core Pattern**

* All docks inherit `DockablePanel` (libs/gui/widgets/).
* Implement `buildUi()` pure virtual + `onSymbolChanged()` hook.
* Register in `MainWindowGPU` constructor + menu + default layout.
* Persistent state via `LayoutManager` (QSettings-based).

### **Widget Communication**

* Hub-and-spoke via `MainWindowGPU` signals.
* `ServiceLocator` for shared services (MarketDataCoreQt, IGridDataSource).
* All cross-thread: Qt::QueuedConnection only.
* No direct widget-to-widget calls.

### **Current Widgets**

* `HeatmapDock` - QML GPU rendering + embedded symbol controls
* `OrderBookDock` - Placeholder (inert; wired later via server)
* `SecFilingDock` - SEC filings viewer
* `CopenetFeedDock` / `AICommentaryFeedDock` - Commentary feeds
* `StatusBar` - Bottom metrics bar (CPU/GPU/Latency)

### **Thread Safety**

* GUI widgets on main thread only.
* MarketDataCoreEngine on worker threads.
* QML rendering on separate render thread.

### **Remote Heatmap Invariants**

* Server is the only producer of heatmap columns.
* Client must not emit local LTSE columns in remote mode.
* Timeframe is locked by `SENTINEL_HEATMAP_TF` (server + client).
* Heatmap grid height and tick size are authoritative from server.
* Client requires server connection (no local-only mode).

---

# **6. Rendering Strategies (You Only Need These Laws)**

* Runs on render thread → never touch GUI QObjects.
* Preallocate QSGGeometry; reuse nodes.
* Validate inputs; skip NaNs/infinite.
* Respect viewportVersion for rebuild logic.
* Keep geometry minimal; avoid child-node explosions.

---

# **7. Testing & Performance (Realistic)**

### **Real Expectations**

* Run the app.
* Look for hitches, stalls, dropped frames.
* Log suspicious behavior (`sentinel.debug`).
* Add tests only where needed (cache & dispatch).

### **Hot Paths**

* DataProcessor
* HeatmapTwapStreamer
* HeatmapLabelRenderer
* QSG Geometry updates

These matter. Everything else is negotiable.

---

# **8. AI Assistant Behavior**

### **Assistants MUST:**

* Follow these rules strictly.
* Keep code clear, modern, and idiomatic.
* Not introduce unnecessary abstractions or enterprise patterns.
* Use your real architecture — never hallucinate systems.

### **Assistants MUST NOT:**

* Generate over-engineered patterns.
* Escalate rules beyond what’s in THIS file.
* Invent fake CI/CD steps or workflows you do not have.
* Enforce pointless constraints (LOC ceilings, verbose PR formats).

Your speed > their bureaucracy.

---

# **9. Task Tracking (`docs/TODO.md`)**

Single source of truth for open work. Read it at session start. Update it as you go.

### **Structure**

* Each feature is a block: `### F<N>: <Name>` with Status, Created, Updated fields.
* Tasks are tiered: **Now** (active focus), **Next** (queued), **Later** (backlog).
* **Done** section holds completed tasks with dates. Never delete finished tasks.
* **Session log** is append-only — one line per session with what happened and what's next.

### **Agent Rules**

* At session start: read `docs/TODO.md`, identify active features relevant to current work.
* When you finish a task: check the box `[x]`, move it to Done with today's date.
* When pausing or ending a session: update the session log and set Updated date.
* If the user pivots to a new feature: append a new `F<N>` block at the bottom.
* Do NOT reorder, renumber, or restructure existing feature blocks.
* Keep Now short (3–5 items). Promote from Next when Now is clear.
* If a feature is fully complete, set its Status to `done`.

---

# **10. References**

* `docs/ARCHITECTURE.md` - Overall system design
* `docs/MARKETDATA.md` - MarketDataCoreEngine pipeline
* `docs/TODO.md` - Feature tracker & session log

---

# **End of AGENTS.md**
