/*
Sentinel — ChartModeController
Role: Implements the logic for the chart mode and settings controller.
Inputs/Outputs: Implements property setters that update state and emit signals on change.
Threading: All code is executed on the main GUI thread.
Performance: Not applicable; consists of simple property setters.
Integration: The concrete implementation of the bridge between QML UI and C++ settings.
Observability: No internal logging.
Related: ChartModeController.h.
Assumptions: '...Changed' signals are connected to slots in other components.
*/
#include "ChartModeController.h"

void ChartModeController::setMode(ChartMode mode) {
    if (m_currentMode == mode) return;
    m_currentMode = mode;
    emit modeChanged(mode);
    updateComponentVisibility();
}

void ChartModeController::updateComponentVisibility() {
    emit componentVisibilityChanged("tradeScatter", m_currentMode == ChartMode::TRADE_SCATTER);
    emit componentVisibilityChanged("candles", m_currentMode == ChartMode::HIGH_FREQ_CANDLES ||
                                        m_currentMode == ChartMode::TRADITIONAL_CANDLES);
    emit componentVisibilityChanged("orderBook", m_currentMode == ChartMode::ORDER_BOOK_HEATMAP);
}
