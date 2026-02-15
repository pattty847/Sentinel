## **AGENTS.md â€” Sentinel Unified Assistant Prompt (v5.3)**

**Source of truth for all AI assistants working on Sentinel.**
Claude, Gemini, Cursor, ChatGPT must follow this file.

Machine: Windows 11
Build: cmake --build --preset windows-msvc-vs

---

# **0. Agent Memory & Workflow**

Path: `_agent/` (AI-only scratchpad; gitignored; concise and standardized).

**Core loop (non-trivial sessions)**
* Check-in by mode (load minimum context first):
  * Lite mode (questions, tooling, docs-only, no code edits): do not preload `_agent/` or `docs/TODO.md`.
  * CodeChange mode (about to implement/refactor): read `_agent/INVARIANTS.md` only.
  * Debug/Perf/Regression mode: read `_agent/INVARIANTS.md` + `_agent/FAILURE_MODES.md`.
* Debugging: assume known failure mode first; scan `_agent/FAILURE_MODES.md` before inventing new theories.
* Design/perf tripwire: state per-frame risk (allocations, object creation, signal emissions, binding re-evals); if uncertain, reduce scope or add guardrails.
* Context hygiene: start with `rg -n` / `rg --files`; prefer targeted line windows; avoid deep reads unless needed.
* Context budget: before first concrete action, read at most ~200 lines total unless the user asks for deep review.
* Escalation rule: if the request is ambiguous, start in Lite mode and only escalate context when needed.
* Check-out: if new invariant/failure mode/decision was discovered, update `_agent/` before stopping.

**Behavior constraints**
* Follow this file strictly; keep code clear, modern, and idiomatic.
* Do not introduce unnecessary abstractions or over-engineered patterns.
* Use the real architecture; never hallucinate systems or invent fake CI/CD workflows.
* Do not escalate rules beyond this file or enforce pointless process constraints.

**_agent files + format**
* `_agent/INVARIANTS.md`: `- INV-### | <statement>`
* `_agent/FAILURE_MODES.md`: `- FM-### | Symptom: <...> | Root: <...> | Guardrail: <...>`
* `_agent/REPO_MAP.md`: `- AREA: <path> | Owns: <...> | Touch with: <...> | Notes: <...>`
* `_agent/DECISIONS.md`: `- YYYY-MM-DD | Decision | Why: <...> | Rejected: <...>`

**_agent rules**
* Keep entries one line, ASCII only; do not add new files unless asked.
* Decision pruning: when a feature ships, condense phase decisions to 1-3 durable principles.
* Qdrant indexing must be directory-targeted only (`libs/`, `docs/`, optional specific source dirs), never repo root; never index `build/` artifacts.
* Human-owned backlogs may live in notes; use them when needed.

**System context**
* Goal: GPU-first, deterministic, zero-lag trading terminal.
* Future: AI commentary (CopeNet), equity/crypto data (including yfinance), richer overlays.
* Keep prompts/notes short and enforceable; do not derail for token usage.

---

# **0b. Obsidian Vault (Long-Form Shared Memory)**

Path: `C:\Users\Pepe\Documents\Programming\Sentinel Archive\Sentinel-Vault`
README: `C:\Users\Pepe\Documents\Programming\Sentinel Archive\Sentinel-Vault\README.md`

Use this vault for long-form memory shared by human + agent: brain dumps, postmortems, bug/fix writeups, refactor blueprints, and session narratives.

**Division of responsibility**
* `_agent/` stays concise and operational (one-line invariants/failure modes/decisions).
* Vault notes hold detailed context, rationale, repros, fixes, tradeoffs, and follow-up ideas.
* If both are updated, write vault first (full detail), then distill durable guardrails into `_agent/`.

**When to write to the vault**
* After debugging a non-trivial issue (especially when repro/fix steps matter).
* When creating or revising plans/refactor blueprints.
* When a decision needs deeper rationale than `_agent/DECISIONS.md` can hold.
* When the user runs a dedicated archive command/prompt (for example from `.cursor/commands`).

**Vault note conventions (from vault README)**
* Use Obsidian wikilinks for cross-references.
* Keep tags small/consistent; prefer links over tag sprawl.
* Follow naming patterns for Sessions/Decisions/Invariants/Pitfalls.
* Use YAML frontmatter (`type`, `project`, `created`, `updated`, `tags`) for discoverability.
* Every session note should link at least one DEC/PIT/INV note when relevant.

**Agent behavior**
* Do not replace `_agent/` workflow; extend it with vault long-form memory.
* Keep vault entries readable, concrete, and solution-oriented (enough detail to reverse steps later).
* Avoid storing secrets/credentials in vault notes.

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
Server: Transport â†’ Dispatch â†’ MarketDataCoreEngine â†’ ServerDataModel â†’ HeatmapTwapStreamer
        â†’ SentinelStreamServer â†’ WebSocket
Client: SentinelStreamClient â†’ RemoteGridDataSource â†’ DataProcessor â†’ UnifiedGridRenderer
        â†’ HeatmapIntensityNode + MsdfGlyphNode â†’ GPU
```

### **Invariant**

**Zoom/pan must always update via `setViewport()` so viewportVersion increments.**
Never mutate viewport fields directly.
If viewportVersion doesnâ€™t change, the grid wonâ€™t rebuild.

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
     What system capability does this enable? What breaks if itâ€™s removed?
  2. Invariant
     â€œThis file guarantees that ___ always ___, even when ___.â€
  3. Ownership
     What it owns (time/state/GPU/threading/I/O/etc) and what it explicitly does not.
  4. Contract
     Inputs (trusted?) and outputs (guarantees?).
  5. Dependency Direction
     Who depends on it, and who it depends on. Any inversion/leak?
  6. Design Choice
     One plausible alternative + why this design wins.
     If unclear: UNKNOWN â€” REVISIT.
  7. Hot vs Cold
     Identify hot path blocks vs setup/glue paths.
  8. State Flow
     Who mutates state, who observes it, where it is cached/derived/authoritative.
  9. Smell Tags
     Tag anything â€œsloppy poopyâ€ as
     SMELL â€” NEEDS CONTEXT or SMELL â€” PROBABLY WRONG.
  10. Confidence
     Can I explain it from memory? If not, list gaps.

### **Threading Rules**

* Network & data processing off GUI thread.
* Cross-thread communication only via Qt::QueuedConnection.
* QSG strategies never touch QObject graph.

### **File Size**

* No arbitrary LOC limit.
* If file feels unwieldy, split when *you* feel itâ€™s time.

---

# **4. Branch Workflow (Non-Bureaucratic)**

### **Branches**

* `main` â€” stable, demo-ready.
* `dev` â€” messy high-velocity work.
* `feature/<name>` â€” only for large refactors.

### **Rules**

* Rebase feature onto dev frequently.
* Donâ€™t stack branches.
* Commit groups of logical feats; clean history optional.

If you can understand your commit messages tomorrow morning, theyâ€™re good.

**GUI Screenshot API**  
Exposes a local HTTP endpoint for automated screenshots of the UI.

- **Endpoint:** `GET http://127.0.0.1:<PORT>/screenshot`
- **Params:**
    - `target`: What to captureâ€”`main` (default), `heatmap`, or `lab`
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
  Run app â†’ modify UI â†’ call screenshot API â†’ PNG saved â†’ review.

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

* Runs on render thread â†’ never touch GUI QObjects.
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

Primary behavior rules are defined in **Section 0 (Agent Memory & Workflow)**.
Act fast, stay practical, and protect Sentinel invariants over process ceremony.

---

# **9. Task Tracking (`docs/TODO.md`)**

Single source of truth for open work. Read it when task tracking is relevant, then update it as you go.

### **Structure**

* Each feature is a block: `### F<N>: <Name>` with Status, Created, Updated fields.
* Tasks are tiered: **Now** (active focus), **Next** (queued), **Later** (backlog).
* **Done** section holds completed tasks with dates. Never delete finished tasks.
* **Session log** is append-only â€” one line per session with what happened and what's next.

### **Agent Rules**

* Read `docs/TODO.md` only when one of these is true:
  * the user asks for TODO/feature/session-log updates
  * the task changes feature scope/priorities
  * the session is ending and task/session log updates are needed
* If read: use targeted section reads (`rg -n "### F<id>|Status|Now|Next|Later|Done|Session log"`), not full-file dumps.
* If asked: identify active features relevant to current work.
* When you finish a task: check the box `[x]`, move it to Done with today's date.
* When pausing or ending a session: update the session log and set Updated date.
* If the user pivots to a new feature: append a new `F<N>` block at the bottom.
* Do NOT reorder, renumber, or restructure existing feature blocks.
* Keep Now short (3â€“5 items). Promote from Next when Now is clear.
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

* Donâ€™t `beginResetModel()` during pan/zoom for QML Repeaters.
* Donâ€™t tie label density directly to data tick size.
* Donâ€™t assume GPU bottleneck when QML churn is present.
* Donâ€™t bind per-label pan offsets in QML; move the math to the model.

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


