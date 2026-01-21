# Layout Review Notes

Scope: LayoutOrchestrator, LayoutManager.

## Findings

1) Duplicate dock insertion
- LayoutOrchestrator::addDocksToLayout adds `secDock` twice to the right dock area.
- Risk: redundant calls or undefined behavior in docking layout; may cause warnings.
- Fix: remove the duplicate addDockWidget call for secDock.
- Files: libs/gui/mainwindow/LayoutOrchestrator.cpp

## Notes

- No other actionable issues found in this group.
