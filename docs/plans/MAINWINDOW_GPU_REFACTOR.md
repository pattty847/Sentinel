# MainWindowGPU Refactor Plan (Minimal, GUI-Aligned)

## Goals
- Reduce responsibility mix in `MainWindowGPU` without adding heavy abstractions.
- Keep GUI ownership of Qt/QML constructs.
- Preserve remote-only data source invariant and existing initialization order.

## Non-Goals
- Introduce multiple data source implementations or local-only modes.
- Move layout dialogs or UI prompts into separate classes without need.

## Plan

### 1) Extract Market Data Wiring Into a GUI Helper
**Create** `libs/gui/mainwindow/MarketDataOrchestrator.{h,cpp}` (QObject).

Responsibilities:
- Encapsulate the signal/slot wiring currently in `connectMarketDataSignals()`.
- Handle connection status updates (`onConnectionStatusChanged`).
- Surface DataSource errors into logs.

Inputs (stored as raw pointers, lifetime owned by `MainWindowGPU`):
- `IGridDataSource*`
- `QmlSceneController*`
- `StatusBar*`
- `QPushButton*` (subscribe/connect button)

Ownership:
- Parent the orchestrator to `MainWindowGPU` to align with Qt lifetimes.

### 2) Keep Subscription Validation in `MainWindowGPU`
- Keep `onSubscribe()` in `MainWindowGPU` to validate symbol input and update QML context.
- Delegate only the `subscribe(symbol)` call to the orchestrator (simple pass-through).

### 3) Leave Layout Dialogs in `MainWindowGPU`
- `onSaveLayout` / `onRestoreLayout` remain as-is unless they grow substantially.

### 4) Optional: Host/Port Config (No Factory)
If configuration is needed:
- Read `SENTINEL_STREAM_HOST` / `SENTINEL_STREAM_PORT` during `RemoteGridDataSource` construction.
- Update `docs/ENV_VARS.md` accordingly.
- Keep `RemoteGridDataSource` as the only implementation.

## Notes / Risks
- Preserve creation order: QML controller must load QML and provide `UnifiedGridRenderer` before wiring signals.
- Keep all cross-thread signal connections using `Qt::QueuedConnection`.
- Ensure orchestrator is destroyed before data source/QML controller (QObject parent does this).

## Acceptance
- `MainWindowGPU` no longer owns `connectMarketDataSignals()` or connection status updates.
- Existing runtime behavior remains unchanged (subscription, rendering, status bar updates).
- No new local data source mode introduced.
