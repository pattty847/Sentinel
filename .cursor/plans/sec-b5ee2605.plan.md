<!-- b5ee2605-de23-42a7-9d34-110437f1de27 b63201a7-f2c1-40d1-9083-22b6bb251064 -->
# SEC Viewer & Dockable Widgets Merge-Readiness Plan

## Scope

Focus this PR on **must-fix, low-risk items** needed before merging the dockable-widgets branch into `main`:

- Replace brittle inline Python in `SecApiClient` with calls to the existing SEC scripts.
- Fix duplicate symbol propagation in `MainWindowGPU::propagateSymbolChange`.
- Optionally adjust CMake widget/theme source listing; leave heavier SEC viewer UX and process-management work for a follow-up branch (guided by `sec-backend-integration-review-refactor-aea828cd.plan.md`).

## 1. SEC Inline Python → Script Calls (Minimal Refactor)

**Files:**

- `libs/gui/widgets/SecApiClient.cpp`
- `libs/gui/widgets/SecApiClient.hpp`

**Goals for this PR:**

- Stop embedding multi-line Python in C++ `QString` literals.
- Use the tested scripts (`scripts\sec\sec_fetch_filings.py`, `scripts\sec\sec_fetch_transactions.py`, `scripts\sec\sec_fetch_financials.py`) to fetch data.
- Preserve current user-visible behavior and JSON parsing; defer advanced process management and UI polish to the later SEC-focused branch.

**Key changes:**

1. **Remove inline Python command construction:**

- Replace the `QString command = "import sys, json, asyncio; ..."` blocks in:
- `SecApiClient::initializePython()` (or remove `initializePython()` entirely if we no longer need a readiness probe).
- `SecApiClient::fetchFilings()`
- `SecApiClient::fetchInsiderTransactions()`
- `SecApiClient::fetchFinancialSummary()`

2. **Introduce script-based execution helper:**

- Add a small helper in `SecApiClient` to build the script path and arguments, e.g.:
- `QString scriptPath = QDir::current().absoluteFilePath("scripts/sec_fetch_filings.py");`
- `QStringList args; args << scriptPath << ticker << formTypeArg;`
- Update `executePythonCommand` (or create a `startScriptProcess(const QString& script, const QStringList& args, const QString& operation)`) to:
- Invoke `pythonExe` **without** `-c`, passing the script path as the first argument and ticker/formType as subsequent args.

3. **Adjust readiness logic for minimal safety:**

- Remove `m_pythonReady` / `initializePython()` or repurpose readiness to a simple script-existence check:
- If `QFileInfo::exists(scriptPath)` is false, emit `apiError("SEC scripts not found")` and return.
- Keep `isReady()` (if present) simple: essentially always ready as long as scripts exist.

4. **Keep existing output markers and JSON parsing:**

- Ensure each script prints the expected prefixes (which `sec_fetch_filings.py` already does):
- `FILINGS_DATA:`
- `TRANSACTIONS_DATA:`
- `FINANCIALS_DATA:`
- Keep the existing `parseFilingsData`, `parseTransactionsData`, and `parseFinancialsData` logic unchanged in this PR.

5. **Defer heavier improvements to follow-up SEC branch:**

- Leave the following items for the dedicated SEC viewer refactor branch (per the existing plan file):
- Timeouts and operation state/queuing.
- Rich error JSON parsing and incremental output parsing.
- Button state management and progress indicators in `SecFilingDock`.
- Rich HTML financials display and visualization features.

## 2. Symbol Propagation Cleanup in `MainWindowGPU`

**Files:**

- `libs/gui/MainWindowGpu.cpp`

**Current issue:**

- `propagateSymbolChange` both emits `symbolChanged(symbol)` **and** manually iterates `findChildren<DockablePanel*>()` to call `dock->onSymbolChanged(symbol)`, while docks like `SecFilingDock` and `MarketDataPanel` are already connected to `symbolChanged` via signals/slots in `setupUI()`.
- This makes some docks update twice on symbol change.

**Change:**

- Simplify `propagateSymbolChange` to rely solely on the signal:
- Replace the body with just `emit symbolChanged(symbol);`.
- Verify that all symbol-aware docks are connected via `connect(this, &MainWindowGPU::symbolChanged, ...)` (which is already done for `m_secDock` and `m_marketDataDock`).

## 3. CMake `file(GLOB ...)` vs Explicit File Lists (Optional for This PR)

**File:**

- `libs/gui/CMakeLists.txt`

**Current state:**

- Widgets and themes are collected with:
- `file(GLOB WIDGET_SOURCES "widgets/*.cpp")`
- `file(GLOB THEME_SOURCES "themes/*.cpp")`
- This tells CMake at **configure time**: “scan this directory and put all matching `.cpp` files into the list.” If you later add a new file, CMake may not notice until you re-run configuration.

**Options:**

1. **If we address it in this PR (small, non-functional change):**

- Replace the GLOBs with explicit `set` lists, aligning with the reviewer’s suggestion and the style already used for `GRID_CORE_SOURCES`:
- Enumerate known widget files (as in the review snippet: `DockablePanel.cpp`, `LayoutManager.cpp`, `MarketDataPanel.cpp`, `SecFilingDock.cpp`, etc.).
- Enumerate known theme files (`DarkTheme.cpp`, `ThemeManager.cpp`).
- This makes build behavior more predictable and ensures new files are always added intentionally.

2. **If we defer to a later cleanup PR:**

- Keep the GLOBs for now, treating this as a **style/maintenance** issue rather than a functional blocker.
- Document in a brief code comment near the GLOBs that you plan to switch to explicit lists in a future build-system cleanup.

**Recommendation:**

- Treat this as **optional** for the dockable-widgets merge: fix it now if you’re comfortable touching CMake in this PR; otherwise, explicitly defer.

## 4. Header Includes vs Forward Declarations (Defer)

**File:**

- `libs/gui/MainWindowGpu.h`

**Reviewer suggestion:**

- Replace direct `#include "widgets/..."` usage in the header with forward declarations for the widget classes, and move the includes into `MainWindowGpu.cpp` to reduce build coupling.

**Plan:**

- Because this is non-functional and there is an existing comment hinting these includes were added to satisfy MOC/build behavior, defer this change to a focused cleanup PR:
- Future work: try forward declarations (`class HeatmapDock;` etc.), move includes to `.cpp`, re-run CMake/moc to ensure everything still builds cleanly on all platforms.

## 5. Order Book Dock Signal Wiring (Explicitly Out of Scope for This PR)

**Files (for later):**

- `libs/gui/widgets/OrderBookDock.cpp`
- `MarketDataCore` + relevant signals

**Current state:**

- The `connect` for order book updates in `OrderBookDock` is commented out with a TODO, and Gemini flagged it as incomplete.

**Plan for this PR:**

- Leave the existing TODO and commented-out `connect` **as-is** in this branch.
- After merging to `main`, create a dedicated branch to:
- Finalize the order book signal in `MarketDataCore` (e.g., `liveOrderBookUpdated` or similar).
- Wire it into both the render path (`DataProcessor`) and the `OrderBookDock` widget.

This keeps the current PR focused on stabilizing the dockable framework and SEC viewer plumbing, while deferring feature-completion work to clearly scoped follow-up branches.

### To-dos

- [ ] Refactor SecApiClient to stop using inline Python and instead call scripts/sec_fetch_*.py with arguments, keeping existing JSON parsing and behavior.
- [ ] Simplify MainWindowGPU::propagateSymbolChange to only emit the symbolChanged signal and rely on signal/slot connections.
- [ ] Either replace WIDGET_SOURCES and THEME_SOURCES file(GLOB ...) calls with explicit file lists in libs/gui/CMakeLists.txt or explicitly defer this cleanup.