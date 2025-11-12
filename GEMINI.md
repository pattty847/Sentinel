# Repository Guidelines

You are an AI coding assistant.
Write correct, efficient, and idiomatic code.
Provide explanations only when requested.
Follow the user’s language and libraries.
Use best practices for readability and performance.

## Mission & Scope
Sentinel is a C++20 trading terminal with a hard separation between pure core logic, Qt-based adapters, and thin app entry points. Every contribution should preserve low-latency data handling, GPU-driven visualization, and modular maintainability.

## Architecture Boundaries & Constraints
- Keep `libs/core` free of Qt (QtCore only), `libs/gui` as adapter/render layers, and `apps/` limited to bootstrapping.
- Treat 500 LOC as the soft ceiling (warn above 450); crossing 500 requires a refactor plan or justification.
- Functions should be kept concise and modular, minimizing complex or deeply nested branching logic.
- Respect thread ownership: network/IO off the GUI thread, UI updates via queued signals, lock-free queues for cross-thread handoffs.
- Use the categorized logging system (`SentinelLogging.hpp/.cpp`) with `sentinel.app|data|render|debug`; avoid temporary or secret-bearing logs.

## Project Layout
- `libs/core/marketdata`: Transport, dispatcher, auth, cache, and sink adapters for the data pipeline.
- `libs/gui`: UnifiedGridRenderer façade, GridViewState, DataProcessor, render strategies, and QML controls.
- `apps/`: `sentinel_gui` and `stream_cli` entry points only.
- `tests/`: Legacy suites exist, but only `tests/marketdata` is currently wired into CMake. Stage new coverage here until the broader matrix is revived.
- `docs/`: Architecture and refactor guidance (`docs/ARCHITECTURE.md`, `docs/sockets-auth-cache-refactor.md`, `docs/LOGGING_GUIDE.md`).
- `scripts/`: Tooling and code-analysis helpers (`scripts/README_CODE_ANALYSIS.md`).

## Build & Test Workflow
- Configure/build via `cmake --preset <preset>` followed by `cmake --build --preset <preset>` (mac-clang, linux-gcc, windows-mingw).
- Run active tests with `cmake --build --preset <preset> --target marketdata_tests` or `ctest --preset <preset> --output-on-failure`.
- Launch the GUI using `./build-<preset>/apps/sentinel_gui/sentinel_gui`.
- Need a snapshot of a large file before diving in? Use the analysis utilities (`quick_cpp_overview.sh`, `extract_functions.sh`) documented in `scripts/README_CODE_ANALYSIS.md` for a quick pass at function lists or structure.

## Coding Style & Expectations
- Favor modern C++20 idioms: RAII, smart pointers, standard library first; reach for Qt types only at integration seams.
- Formatting: 4-space indent, Qt brace style for classes, headers as `.hpp`; classes/enums in `PascalCase`, methods/signals `camelCase`, private members `m_`, constants `kPascalCase`.
- Keep cross-domain DTOs lightweight and avoid business logic in GUI layers.
- Comment intent when the domain is subtle; otherwise rely on expressive naming.

## Quality Gates
- No pull request without a passing build and `ctest`.
- Add or update tests with the code change; keep runs deterministic (mock transports, reuse fixtures).
- Watch for performance regressions on hot paths and call them out in PR notes if unavoidable.
- Document migrations, manual steps, and UI screenshots in commits/PRs, following the `Component: concise change` convention.

## References
- Architecture overview: `docs/ARCHITECTURE.md`
- Market data refactor guide: `docs/sockets-auth-cache-refactor.md`
- Logging details: `docs/LOGGING_GUIDE.md`
- Code analysis tooling: `scripts/README_CODE_ANALYSIS.md`

---

## Rendering invariant: Zoom must bump `viewportVersion`

- **What**: Any zoom operation must update the viewport via `GridViewState::setViewport(...)` so `m_viewportVersion` increments and `viewportChanged` is emitted.
- **Why**: `DataProcessor::updateVisibleCells()` gates full rebuilds on the viewport version. If zoom code writes bounds directly and skips `setViewport`, the version won’t change; the processor won’t rebuild, so zooming out won’t reveal history until a later pan triggers a version bump.
- **How**: Route zoom math in `GridViewState::handleZoomWithViewport(...)` to `setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice)` rather than assigning members. Do not emit `viewportChanged` manually; the setter handles both versioning and signaling.

Code reference — zoom path uses `setViewport(...)`:
```167:185:libs/gui/render/GridViewState.cpp
            // Apply validated bounds via central setter to bump version and signal change
            setViewport(newTimeStart, newTimeEnd, newMinPrice, newMaxPrice);
            
            if constexpr (kTraceZoomInteractions) {
                qDebug() << " ZOOM RESULT:"
                         << "OldTime[" << (m_visibleTimeStart_ms + static_cast<int64_t>(currentTimeRange * centerTimeRatio)) << "]"
                         << "NewTime[" << currentCenterTime << "]"
                         << "TimeRange:" << currentTimeRange << "->" << newTimeRange;
            }
        }
        
        m_zoomFactor = newZoom;
        
        // Disable auto-scroll when user manually zooms
        if (m_autoScrollEnabled) {
            m_autoScrollEnabled = false;
            emit autoScrollEnabledChanged();
        }
```

Code reference — processor rebuilds only when the viewport version changes:
```413:421:libs/gui/render/DataProcessor.cpp
    // Viewport version gating: full rebuild only when viewport changes
    const uint64_t currentViewportVersion = m_viewState->getViewportVersion();
    const bool viewportChanged = (currentViewportVersion != m_lastViewportVersion);
    if (viewportChanged) {
        m_visibleCells.clear();
        m_lastProcessedTime = 0;
        m_lastViewportVersion = currentViewportVersion;
    }
```

### Event pipeline (zoom → repaint)
- `GridViewState::handleZoom*` computes new bounds → calls `setViewport(...)`.
- `GridViewState::setViewport` increments `m_viewportVersion` and emits `viewportChanged`.
- `UnifiedGridRenderer::onViewportChanged` enqueues `DataProcessor::updateVisibleCells()` (queued connection) and marks `m_transformDirty`.
- `DataProcessor::updateVisibleCells` detects `viewportChanged` via version, rebuilds, publishes snapshot, emits `dataUpdated`.
- `UnifiedGridRenderer` receives `dataUpdated`, sets `m_appendPending`, calls `update()`; `updatePaintNode` pulls the snapshot and updates geometry/content.

### Testing checklist
- Zoom out with mouse wheel: historical rows/columns should appear immediately (no pan required).
- Logs should show processor “rebuild” when zooming, not just “append”.
- Pan end should still trigger a resync (queued `updateVisibleCells`) to clear any transient visual offset.

### Pitfalls
- Avoid any direct writes to `GridViewState` viewport members in interaction code.
- Do not manually emit `viewportChanged` from zoom handlers; always go through `setViewport`.

Last updated: 2025-11-05
