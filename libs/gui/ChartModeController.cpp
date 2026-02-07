/*
Sentinel — ChartModeController
Role: Implements primary field selection and overlay toggles for the chart.
Threading: All code is executed on the main GUI thread.
Integration: The concrete implementation of the bridge between QML UI and C++ settings.
Related: ChartModeController.h.
*/
#include "ChartModeController.h"

int ChartModeController::primaryField() const {
    return static_cast<int>(m_primaryField);
}

void ChartModeController::setPrimaryField(PrimaryField field) {
    if (m_primaryField == field) {
        return;
    }
    m_primaryField = field;
    emit primaryFieldChanged(static_cast<int>(m_primaryField));
}

void ChartModeController::setPrimaryField(int field) {
    if (field < static_cast<int>(PrimaryField::Heatmap) ||
        field > static_cast<int>(PrimaryField::FootprintCells)) {
        return;
    }
    setPrimaryField(static_cast<PrimaryField>(field));
}

void ChartModeController::setCandlesEnabled(bool enabled) {
    if (m_candlesEnabled == enabled) {
        return;
    }
    m_candlesEnabled = enabled;
    emit candlesEnabledChanged(m_candlesEnabled);
}
