## **AGENTS.md — Sentinel Unified Assistant Prompt (v5.1)**

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
* Handles: market data transport, cache, dispatchers, DTOs, transforms.

### **GUI Layer (`libs/gui`)**

* Owns Qt, QML, QSG, rendering strategies, widget layouts.
* Contains the docking system & communication between widgets.

### **Apps (`apps/`)**

* Thin bootstraps. No business logic.

### **Render Path**

```
Transport → Dispatch → Cache → DataProcessor → LiquidityEngine
           → UnifiedGridRenderer → QSG Render Strategies → GPU
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

---

# **5. Dockable Widgets (Short Version)**

### **Rules**

* All docks inherit `DockablePanel`.
* Implement `buildUi()` + override `onSymbolChanged()` when needed.
* Register each dock in `MainWindowGPU`.
* State saved via `LayoutManager`.

### **Cross-Dock Communication**

* Use signals/slots (QueuedConnection).
* No direct cross-widget mutation.

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
* LiquidityTimeSeriesEngine
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

# **9. References**

* `docs/ARCHITECTURE.md`
* Dockable framework: `docs/features/aesthetic/DOCKABLE_FRAMEWORK.md`
* Widget comms: `docs/features/aesthetic/WIDGET_COMMUNICATION.md`
* Render strategies: `TradeBubbleStrategy.cpp`, `TradeFlowStrategy.cpp`
* Backend SEC plan (optional): `.cursor/plans/sec-backend-integration-*.md`

---

# **End of AGENTS.md**