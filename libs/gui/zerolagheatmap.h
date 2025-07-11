// libs/gui/zerolagheatmap.h
#pragma once

#include <QQuickItem>
#include <QSGNode>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <unordered_map>
#include <array>
#include <atomic>
#include <mutex>
#include "dynamiclod.h"
#include "unifiedviewport.h"
#include "../core/tradedata.h"

class ZeroLagHeatmap : public QQuickItem {
    Q_OBJECT
    
public:
    struct HeatmapCell {
        int64_t timestamp_ms;
        double price;
        double aggregatedSize;
        float intensity; // 0.0 to 1.0
        uint32_t rgba;   // Packed color for GPU
    };

    using CellBuffer = std::vector<HeatmapCell>;
    struct InstanceData {
        float x, y, width, height;
        uint32_t rgba;
    };


    explicit ZeroLagHeatmap(QQuickItem* parent = nullptr);
    
    // ZERO allocation coordinate transform
    inline QPointF worldToScreen(int64_t timestamp_ms, double price) const {
        return m_viewport->worldToScreen(timestamp_ms, price);
    }
    
    // Immediate LOD switching - no rebuild!
    void switchToTimeFrame(DynamicLOD::TimeFrame newTF);
    
    // Fast data ingestion
    void addOrderBookUpdate(int64_t timestamp_ms, const std::vector<OrderBookLevel>& bids, 
                           const std::vector<OrderBookLevel>& asks);

signals:
    void timeFrameChanged(DynamicLOD::TimeFrame newTF, const DynamicLOD::GridInfo& gridInfo);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private slots:
    void onViewportChanged(const UnifiedViewport::ViewState& state);
    void onTimeFrameChanged(DynamicLOD::TimeFrame newTF, const DynamicLOD::GridInfo& gridInfo);

private:
    std::array<CellBuffer, static_cast<int>(DynamicLOD::TimeFrame::COUNT)> m_lodBuffers;

    UnifiedViewport* m_viewport = nullptr;
    DynamicLOD* m_lod = nullptr;
    DynamicLOD::TimeFrame m_currentTimeFrame = DynamicLOD::TimeFrame::TF_1s;

    // GPU state
    std::atomic<bool> m_geometryDirty{true};
    std::atomic<bool> m_lodDirty{true};
    mutable std::mutex m_bufferMutex;

    // Instanced rendering for maximum performance
    std::vector<InstanceData> m_instances;

    // Pre-allocated geometry for zero-allocation rendering
    QSGGeometry* m_geometry = nullptr;
    QSGVertexColorMaterial* m_material = nullptr;

    void updateGeometry();
    void rebuildInstances();
    uint32_t calculateCellColor(double intensity, bool isBid) const;
    
    // Aggregation engine
    void aggregateToTimeFrame(DynamicLOD::TimeFrame targetTF);
    std::unordered_map<int64_t, std::unordered_map<int, double>> m_rawData; // timestamp -> price_cents -> size
};