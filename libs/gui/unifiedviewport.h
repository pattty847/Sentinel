// libs/gui/unifiedviewport.h
#pragma once

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <chrono>
#include <atomic>

class UnifiedViewport : public QObject {
    Q_OBJECT
    
public:
    struct ViewState {
        int64_t timeStart_ms;
        int64_t timeEnd_ms;
        double priceMin;
        double priceMax;
        double pixelWidth;
        double pixelHeight;
        
        // Derived properties for performance
        double timeRange_ms() const { return timeEnd_ms - timeStart_ms; }
        double priceRange() const { return priceMax - priceMin; }
        double msPerPixel() const { return timeRange_ms() / pixelWidth; }
        double pricePerPixel() const { return priceRange() / pixelHeight; }
    };
    
    explicit UnifiedViewport(QObject* parent = nullptr);
    
    // FAST coordinate transformation (no allocations)
    inline QPointF worldToScreen(int64_t timestamp_ms, double price) const {
        const auto& state = m_currentState.load();
        double x = (timestamp_ms - state.timeStart_ms) / state.timeRange_ms() * state.pixelWidth;
        double y = (1.0 - (price - state.priceMin) / state.priceRange()) * state.pixelHeight;
        return QPointF(x, y);
    }
    
    inline QPointF screenToWorld(const QPointF& screen) const {
        const auto& state = m_currentState.load();
        int64_t timestamp = state.timeStart_ms + (screen.x() / state.pixelWidth) * state.timeRange_ms();
        double price = state.priceMin + (1.0 - screen.y() / state.pixelHeight) * state.priceRange();
        return QPointF(timestamp, price);
    }
    
    // Immediate viewport updates (no lag)
    void setViewport(int64_t timeStart, int64_t timeEnd, double priceMin, double priceMax);
    void setPixelSize(double width, double height);
    void pan(double deltaX, double deltaY);
    void zoom(double factor, const QPointF& center);
    
    const ViewState& currentState() const { return m_currentState.load(); }
    
signals:
    void viewportChanged(const ViewState& newState);
    
private:
    std::atomic<ViewState> m_currentState;
    
    void emitChange();
};