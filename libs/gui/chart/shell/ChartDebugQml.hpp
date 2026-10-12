#pragma once
#include <QObject>
#include <QString>
class GridViewState;
namespace chart {
struct ChartLayers;
class PublishedFrame;
class ChartDebugQml : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool showGpuStatsOverlay READ showGpuStatsOverlay WRITE setShowGpuStatsOverlay NOTIFY showGpuStatsOverlayChanged)
    Q_PROPERTY(bool showDataPipelineOverlay READ showDataPipelineOverlay WRITE setShowDataPipelineOverlay NOTIFY showDataPipelineOverlayChanged)
    Q_PROPERTY(bool showViewportMathOverlay READ showViewportMathOverlay WRITE setShowViewportMathOverlay NOTIFY showViewportMathOverlayChanged)
public:
    void bind(ChartLayers& layers, const PublishedFrame& published, GridViewState* view) { layers_ = &layers; published_ = &published; view_ = view; }
    bool showGpuStatsOverlay() const { return flags_.gpuStats; }
    bool showDataPipelineOverlay() const { return flags_.dataPipeline; }
    bool showViewportMathOverlay() const { return flags_.viewportMath; }
    Q_INVOKABLE QString getViewportMathDebug() const;
    Q_INVOKABLE QString getDataPipelineDebug() const;
    Q_INVOKABLE QString getPerformanceStats() const;
    Q_INVOKABLE QString getMsdfAtlasMemory() const;
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowViewportMathOverlay(bool show);
signals:
    void showGpuStatsOverlayChanged();
    void showDataPipelineOverlayChanged();
    void showViewportMathOverlayChanged();
private:
    struct DebugFlags { bool gpuStats = false, dataPipeline = false, viewportMath = false; } flags_;
    ChartLayers* layers_ = nullptr;
    const PublishedFrame* published_ = nullptr;
    GridViewState* view_ = nullptr;
};
} // namespace chart
