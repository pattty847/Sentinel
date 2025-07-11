#include "unifiedviewport.h"
#include <chrono>

UnifiedViewport::UnifiedViewport(QObject* parent) : QObject(parent) {
    ViewState initial{};
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    initial.timeEnd_ms = now;
    initial.timeStart_ms = now - 60000; // 1 minute default
    initial.priceMin = 100000.0;
    initial.priceMax = 120000.0;
    initial.pixelWidth = 1920.0;
    initial.pixelHeight = 1080.0;
    
    m_currentState.store(initial);
}

void UnifiedViewport::setViewport(int64_t timeStart, int64_t timeEnd, double priceMin, double priceMax) {
    auto state = m_currentState.load();
    state.timeStart_ms = timeStart;
    state.timeEnd_ms = timeEnd;
    state.priceMin = priceMin;
    state.priceMax = priceMax;
    m_currentState.store(state);
    emitChange();
}

void UnifiedViewport::setPixelSize(double width, double height) {
    auto state = m_currentState.load();
    state.pixelWidth = width;
    state.pixelHeight = height;
    m_currentState.store(state);
    emitChange();
}

void UnifiedViewport::pan(double deltaX, double deltaY) {
    auto state = m_currentState.load();
    double timeDelta = deltaX * state.msPerPixel();
    double priceDelta = deltaY * state.pricePerPixel();
    
    state.timeStart_ms -= static_cast<int64_t>(timeDelta);
    state.timeEnd_ms -= static_cast<int64_t>(timeDelta);
    state.priceMin += priceDelta;
    state.priceMax += priceDelta;
    
    m_currentState.store(state);
    emitChange();
}

void UnifiedViewport::zoom(double factor, const QPointF& center) {
    auto state = m_currentState.load();
    
    // Time zoom around center
    int64_t centerTime = state.timeStart_ms + (center.x() / state.pixelWidth) * state.timeRange_ms();
    double newTimeRange = state.timeRange_ms() * factor;
    state.timeStart_ms = centerTime - static_cast<int64_t>(newTimeRange * center.x() / state.pixelWidth);
    state.timeEnd_ms = state.timeStart_ms + static_cast<int64_t>(newTimeRange);
    
    // Price zoom around center
    double centerPrice = state.priceMin + (1.0 - center.y() / state.pixelHeight) * state.priceRange();
    double newPriceRange = state.priceRange() * factor;
    double priceRatio = (1.0 - center.y() / state.pixelHeight);
    state.priceMin = centerPrice - newPriceRange * priceRatio;
    state.priceMax = state.priceMin + newPriceRange;
    
    m_currentState.store(state);
    emitChange();
}

void UnifiedViewport::emitChange() {
    emit viewportChanged(m_currentState.load());
}