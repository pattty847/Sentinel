# Sentinel UI Roadmap (Charts Focus)

## Current State (Phase 1: UI Shell)
- **Charts dock owns its own toolbar** (chart-specific controls live inside the dock).
- **Global menu bar only** for app-level actions (View/Layouts/Tools/Debug).
- Left control panel removed; chart controls now live in the toolbar only.
- Right watchlist dock exists with placeholder data.
- Dock title bars are visible (stable drag/resize behavior).
- Icons are loaded via Qt resources under `:/icons/*`.
- Fonts can be switched at runtime (system fonts + resource fonts).
- QML theme is bridged to the QWidget theme (shared palette).

## Key Decisions
- Chart/heatmap controls live in the **Charts dock toolbar** (not in-chart panels).
- Liquidity threshold is the only filter exposed for now (volume filter + grid resolution deferred).
- Heatmap dock is renamed to **Charts** and supports mode switching.
- MSDF is for heatmap/charts only; Qt Widgets/QML use standard fonts.

## Next Steps (Suggested)
1) **Toolbar polish**
   - Adjust spacing, typography, and label hierarchy.
   - Add hover tooltips and compact dropdown styling.
   - Optional: dock-specific micro-toolbar for other widgets.
2) **Chart interactions**
   - Live axis updates during pan.
   - CTRL + wheel zoom at cursor.
   - Drag-zoom rectangle.
3) **Watchlist enhancement**
   - Real data wiring + persistence.
   - Column configuration and sections.
4) **SEC/Commentary polish**
   - Clean up integrations and UI consistency.
5) **Theme switching**
   - Add runtime theme selector that updates Qt stylesheet + QML bridge.

## Resource Notes
- Use `QT_RESOURCE_ALIAS` when adding SVGs via CMake so runtime paths stay stable.
- Runtime icon paths are `:/icons/<name>.svg`.
