// libs/gui/zerolagarchitecture.h
#pragma once

#include <QObject>
#include <QQuickItem>
#include <memory>
#include <atomic>
#include "unifiedviewport.h"
#include "dynamiclod.h"
#include "zerolagheatmap.h"
#include "instanttestdata.h"

class ZeroLagArchitecture : public QObject {
    Q_OBJECT
    
public:
    explicit ZeroLagArchitecture(QObject* parent = nullptr);
    
    // Initialize all components with perfect synchronization
    void initialize(QQuickItem* rootItem);
    
    // Start test mode with instant multi-timeframe data
    void startTestMode();
    
    // Connect to real WebSocket data
    void connectRealData();
    
    // Immediate user interaction handling - ZERO LAG!
    void handleMousePress(const QPointF& position);
    void handleMouseMove(const QPointF& delta);
    void handleWheel(double delta, const QPointF& center);
    
private slots:
    void onOrderBookUpdate(const InstantTestData::OrderBookSnapshot& snapshot);
    void onTradeExecuted(int64_t timestamp_ms, double price, double size, bool isBuy);
    void onViewportChanged(const UnifiedViewport::ViewState& state);
    void onTimeFrameChanged(DynamicLOD::TimeFrame newTF, const DynamicLOD::GridInfo& gridInfo);
    
private:
    // Core components
    std::unique_ptr<UnifiedViewport> m_viewport;
    std::unique_ptr<DynamicLOD> m_lod;
    std::unique_ptr<ZeroLagHeatmap> m_heatmap;
    std::unique_ptr<InstantTestData> m_testData;
    
    // Performance monitoring
    std::atomic<int64_t> m_frameCount{0};
    std::atomic<double> m_avgFrameTime{0.0};
    
    // Mouse interaction state
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    
    void setupConnections();
    void optimizePerformance();
};