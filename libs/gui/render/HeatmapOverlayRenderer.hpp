// Render-thread heatmap overlay module used by the chart renderer host.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QRectF>
#include <atomic>
#include <vector>

class QQuickWindow;
class HeatmapIntensityNode;

class HeatmapOverlayRenderer {
public:
    struct PendingUpload {
        int x = 0;
        QByteArray data;
    };

    struct ColorStop {
        float position = 0.0f;
        QColor color;
    };

    void setGridDimensions(int width, int height);
    void setIntensityBytesPerCell(int bytesPerCell);
    void setBackgroundColor(const QColor& color);
    void setPaletteGamma(double gamma);
    void setBidGradient(const std::vector<ColorStop>& stops);
    void setAskGradient(const std::vector<ColorStop>& stops);
    void requestFullTextureRebuild();
    void onRootRebuilt();

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
        QColor interpolate(float t) const;
    };

    void ensureHeatmapImage();
    void ensurePaletteImage();

    int m_gridWidth = 5120;
    int m_gridHeight = 2048;
    int m_intensityBytesPerCell = 1;
    QColor m_backgroundColor = QColor(18, 20, 24);

    bool m_textureDirty = true;
    bool m_paletteDirty = true;
    double m_paletteGamma = 2.0;
    QImage m_heatmapImage;
    QImage m_paletteImage;

    ColorGradient m_bidGradient;
    ColorGradient m_askGradient;
    std::atomic<bool> m_rebuildPending{false};
};
