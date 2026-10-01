// Render-thread heatmap overlay module used by the chart renderer host.
#pragma once

#include "IOverlayRenderer.hpp"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QRectF>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

class HeatmapColumnTexture;
namespace heatmap::gpu { struct PaletteStop; }

class QQuickWindow;
class QSGGeometryNode;
class HeatmapIntensityNode;

class HeatmapOverlayRenderer : public IOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        QByteArray data;
    };

    struct ColorStop {
        float position = 0.0f;
        QColor color;
    };

    static std::vector<ColorStop> toColorStops(const std::vector<heatmap::gpu::PaletteStop>& stops);
    void setGridDimensions(int width, int height);
    void setIntensityBytesPerCell(int bytesPerCell);
    void setBackgroundColor(const QColor& color);
    void setPaletteGamma(double gamma);
    void setBidGradient(const std::vector<ColorStop>& stops);
    void setAskGradient(const std::vector<ColorStop>& stops);
    void setHistoryCoverage(QByteArray coverage);
    void requestFullTextureRebuild();
    void onRootRebuilt() override;
    int zOrder() const override { return 0; }

    void applyToNode(QQuickWindow* window,
                     HeatmapIntensityNode* node,
                     bool drawHeatmap,
                     float gamma,
                     float contrast,
                     float shaderFloor,
                     bool forceFull,
                     float timeOffset,
                     const QRectF& drawRect,
                     const QRectF& srcRect,
                     std::vector<PendingUpload>& pendingUploads);

private:
    struct ColorGradient {
        std::vector<ColorStop> stops;
    };

    void ensureHeatmapImage();
    void ensurePaletteImage();
    void updateHistoryGapNode(HeatmapIntensityNode* root,
                              bool visible,
                              const QRectF& drawRect,
                              const QRectF& srcRect);

    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_intensityBytesPerCell = 1;
    QColor m_backgroundColor = QColor(18, 20, 24);

    bool m_textureDirty = true;
    bool m_paletteDirty = true;
    double m_paletteGamma = 2.0;
    QImage m_heatmapImage;               // CPU mirror for full uploads
    HeatmapColumnTexture* m_columnTexture = nullptr;  // RHI path; owned by the node
    QImage m_paletteImage;

    std::mutex m_historyCoverageMutex;
    QByteArray m_pendingHistoryCoverage;
    std::atomic<bool> m_historyCoverageDirty{false};
    QByteArray m_historyCoverage;
    std::vector<std::pair<int, int>> m_historyGapRuns;
    QSGGeometryNode* m_historyGapNode = nullptr;
    QRectF m_lastGapDrawRect;
    QRectF m_lastGapSourceRect;
    bool m_gapGeometryDirty = true;
    bool m_gapVisible = false;

    ColorGradient m_bidGradient;
    ColorGradient m_askGradient;
    std::atomic<bool> m_rebuildPending{false};
};
