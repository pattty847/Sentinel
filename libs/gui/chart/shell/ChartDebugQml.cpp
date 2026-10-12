#include "ChartDebugQml.hpp"
#include "ChartDebugText.hpp"
#include "chart/layers/ChartLayers.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/GridViewState.hpp"
#include "PerformanceMonitor.hpp"
namespace chart {
void ChartDebugQml::setShowGpuStatsOverlay(bool show) {
  if (flags_.gpuStats != show) { flags_.gpuStats = show; emit showGpuStatsOverlayChanged(); }
}

void ChartDebugQml::setShowDataPipelineOverlay(bool show) {
  if (flags_.dataPipeline != show) { flags_.dataPipeline = show; emit showDataPipelineOverlayChanged(); }
}

void ChartDebugQml::setShowViewportMathOverlay(bool show) {
  if (flags_.viewportMath != show) { flags_.viewportMath = show; emit showViewportMathOverlayChanged(); }
}

QString ChartDebugQml::getMsdfAtlasMemory() const { return layers_->text.getMsdfAtlasMemory(); }

QString ChartDebugQml::getViewportMathDebug() const { return chart::formatViewportMath(published_->current().mapping, view_ && view_->isDragging()); }

QString ChartDebugQml::getDataPipelineDebug() const { return chart::formatDataPipeline(layers_->heatmap.gpu.get()); }

QString ChartDebugQml::getPerformanceStats() const { return PerformanceMonitor::instance().frameStatsText(); }
} // namespace chart
