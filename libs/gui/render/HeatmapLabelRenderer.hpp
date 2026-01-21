/*
Sentinel — HeatmapLabelRenderer
Role: Builds heatmap liquidity label textures from ring snapshots.
Threading: Build on GUI thread; result read on render thread.
*/
#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <atomic>
#include <mutex>
#include <vector>

#include "HeatmapStreamState.hpp"

class HeatmapLabelRenderer {
public:
    struct Request {
        QRectF srcRect;
        QSize labelSize;
        int startX = 0;
        int startY = 0;
        float cellW = 0.0f;
        float cellH = 0.0f;
        int fontPx = 0;
        int fontBucket = 0;
        uint64_t viewportVersion = 0;
        bool valid = false;
    };

    struct Result {
        QImage image;
        int startX = 0;
        int startY = 0;
        QSize pixelSize;
        QSizeF sourceSize;
        int fontBucket = 0;
        uint64_t viewportVersion = 0;
        bool valid = false;
    };

    void buildFromSnapshot(const Request& request,
                           const HeatmapStreamState::LabelSnapshot& snapshot,
                           bool dollars);

    int version() const { return m_version.load(); }
    Result result() const;

private:
    static QString formatLiquidityLabel(double value, bool dollars);

    mutable std::mutex m_mutex;
    Result m_result;
    std::atomic<int> m_version{0};
};
