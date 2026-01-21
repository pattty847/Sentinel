# Axis + Coordinate Review Notes

Scope: AxisModel, TimeAxisModel, PriceAxisModel, CoordinateSystem.

## Findings

1) TimeAxisModel::formatLabel uses viewport range as "step"
- formatLabel() passes the full viewport range to formatTimeLabel(), which expects step sizing.
- Current tick labels are precomputed in calculateTicks(), so this only matters if formatLabel is used directly.
- If formatLabel is used elsewhere (QML or debug), it will format labels with the wrong granularity.
- Files: libs/gui/models/TimeAxisModel.cpp

## Notes

- No other actionable issues found in this group.
