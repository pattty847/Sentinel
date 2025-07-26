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
