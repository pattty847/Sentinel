## **AGENTS.md — Sentinel Unified Assistant Prompt (v5.2)**

**Source of truth for all AI assistants working on Sentinel.**
Claude, Gemini, Cursor, ChatGPT must follow this file.

Machine: Windows 11
Build: cmake --build --preset windows-msvc-vs

---

# **0. Agent Notes (Local Memory)**

Path: `_agent/` (AI-only scratchpad; keep short and standardized).
This folder is gitignored by default.

**Rules**
* Read `_agent/INVARIANTS.md` and `_agent/FAILURE_MODES.md` at session start.
* Update notes at session end if a new invariant, guardrail, or decision was discovered.
* Keep entries one line; no paragraphs; ASCII only.
* Do not add new files unless explicitly requested.
* **DECISIONS.md pruning:** When a feature ships, condense its phase/step decisions into 1-3 surviving principles. Delete entries that are now just "how the code works" with no plausible alternative.
* The human has backlogs in notes if we need them.

**Files + expected format**
* File: `_agent/INVARIANTS.md` | Format: `- INV-### | <statement>`
* File: `_agent/FAILURE_MODES.md` | Format: `- FM-### | Symptom: <...> | Root: <...> | Guardrail: <...>`
* File: `_agent/REPO_MAP.md` | Format: `- AREA: <path> | Owns: <...> | Touch with: <...> | Notes: <...>`
* File: `_agent/DECISIONS.md` | Format: `- YYYY-MM-DD | Decision | Why: <...> | Rejected: <...>`

---

# **0a. Mandatory Agent Behaviors (Low-Ceremony)**

**Invariant Check‑In (start of non‑trivial sessions)**
* Read `_agent/INVARIANTS.md` + `_agent/FAILURE_MODES.md`.
* State which invariants this task relies on and which might be at risk.

**Invariant Check‑Out (end of session)**
* Did we discover a new invariant, failure mode, or decision? If yes, update `_agent/` before stopping.

**Failure‑Mode‑First Debugging**
* Assume the bug is a known failure mode until falsified.
* Scan `_agent/FAILURE_MODES.md` before inventing new theories.

**Decision Crystallization Trigger**
* Log in `_agent/DECISIONS.md` when we choose X over Y, reject a tempting alternative, or freeze a previously flexible choice.

**Performance Tripwire (hot paths)**
* Before coding, state potential per‑frame costs: allocations, object creation, signal emissions, binding re‑evals.
* If uncertain, treat it as a risk and reduce scope or add guardrails.

**Context Hygiene (exploration first)**
* Start with `rg -n` / `rg --files` to narrow scope before opening full files.
* Prefer targeted line windows over full-file reads for large sources.
* If unsure whether deep read is needed, check file size/LOC first and inspect only the relevant sections.
* Qdrant indexing for this refactor must be directory-targeted only: index `libs/` and `docs/` (and optional specific source dirs), never repo root.
* Do not index `build/` artifacts (including Qt Creator multi-kit outputs, moc/autogen, and preset-specific build trees).

**System Context (Sentinel)**
* Goal: GPU‑first, deterministic, zero‑lag trading terminal (ExoCharts + TradingView + SierraCharts + Bookmap + TradingLite vibes).
* Future: AI commentary (CopeNet), equity/crypto data (including free sources like yfinance), and richer overlays.
* Constraint: do not derail for token usage; keep prompts and notes short and enforceable.

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
        → HeatmapIntensityNode + MsdfGlyphNode → GPU
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
* **Comments:** Prefer code as documentation. When you do comment, explain *why* (decisions, invariants, protocol quirks), not *what* (the code already shows that). No COT, filler, or meta-commentary in the codebase. File headers: one short line (role/threading) if needed; no essays.
* Separate concerns: logic in core, visuals in gui.
* Prefer config files over new ENV VARs; only add env vars when truly necessary.
* Qt resources: when adding SVGs/icons via CMake, use `QT_RESOURCE_ALIAS` (or explicit aliases) so runtime paths are stable (e.g., `:/icons/icon-*.svg`). Avoid absolute path aliases.
* Backwards compatibility is optional unless explicitly required in this doc. If a simpler or faster design is better, propose it.
* Performance invariants (GPU-first, `setViewport()`, minimal churn) matter more than preserving legacy patterns.

# **3a. Planning Checklist (New Features & Refactors)**
### **Musts**
  - Before adding a feature or refactoring a file/module, answer these:

  1. Purpose
     What system capability does this enable? What breaks if it’s removed?
  2. Invariant
     “This file guarantees that ___ always ___, even when ___.”
  3. Ownership
     What it owns (time/state/GPU/threading/I/O/etc) and what it explicitly does not.
  4. Contract
     Inputs (trusted?) and outputs (guarantees?).
  5. Dependency Direction
     Who depends on it, and who it depends on. Any inversion/leak?
  6. Design Choice
     One plausible alternative + why this design wins.
     If unclear: UNKNOWN — REVISIT.
  7. Hot vs Cold
     Identify hot path blocks vs setup/glue paths.
  8. State Flow
     Who mutates state, who observes it, where it is cached/derived/authoritative.
  9. Smell Tags
     Tag anything “sloppy poopy” as
     SMELL — NEEDS CONTEXT or SMELL — PROBABLY WRONG.
  10. Confidence
     Can I explain it from memory? If not, list gaps.

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
* Commit groups of logical feats; clean history optional.

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
* Timeframe is locked by server_config (server authoritative).
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

* If asked: read `docs/TODO.md`, identify active features relevant to current work.
* When you finish a task: check the box `[x]`, move it to Done with today's date.
* When pausing or ending a session: update the session log and set Updated date.
* If the user pivots to a new feature: append a new `F<N>` block at the bottom.
* Do NOT reorder, renumber, or restructure existing feature blocks.
* Keep Now short (3–5 items). Promote from Next when Now is clear.
* If a feature is fully complete, set its Status to `done`.

---

# **10. Performance Lessons (QML/Axis)**

### **What Worked**

* Profile early (QML Profiler / CPU profiler) before guessing.
* Keep axis labels pixel-density driven, not data-tick driven.
* Stabilize label count across zoom (nice ticks).
* Use fixed-capacity axis models + role-scoped `dataChanged` to avoid churn.
* During drag, recompute axis range from visual pan, not QML offsets.

### **Never Again**

* Don’t `beginResetModel()` during pan/zoom for QML Repeaters.
* Don’t tie label density directly to data tick size.
* Don’t assume GPU bottleneck when QML churn is present.
* Don’t bind per-label pan offsets in QML; move the math to the model.

---

# **11. Coinbase API Implementation Notes**

* REST candles (Advanced Trade): `GET /api/v3/brokerage/products/{product_id}/candles` with `start`, `end`, `granularity`, `limit` (max 350).
* Granularity mapping is based on timeframe seconds (e.g., 60 -> ONE_MINUTE, 300 -> FIVE_MINUTE).
* REST JWT `uri` claim uses: `METHOD + host + path` (no query string).
* If auth endpoint returns 401, retry public candles endpoint: `/api/v3/brokerage/market/products/{product_id}/candles` (no auth).
* TLS on Windows: load CA bundle from `resources/certs/ca-bundle.crt` or set `SENTINEL_CA_BUNDLE` env var.

---

# **12. References**

* `docs/ARCHITECTURE.md` - Overall system design
* `docs/MARKETDATA.md` - MarketDataCoreEngine pipeline
* `docs/TODO.md` - Feature tracker & session log

---

# **End of AGENTS.md**
