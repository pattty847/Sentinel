#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/RenderSequence.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/ChartTextRenderer.hpp"
#include "SentinelLogging.hpp"
#include <QSGOpacityNode>
#include <QQuickWindow>

namespace chart {
HeatmapLayer::HeatmapLayer() = default;
HeatmapLayer::~HeatmapLayer() = default;
void HeatmapLayer::initialize() {
    gpu = std::make_unique<heatmap::gpu::HeatmapGpuLayer>();
    tileStats_ = gpu->tileStatsPtr();
}
void HeatmapLayer::onRootRebuilt() { gate_ = nullptr; tile_ = nullptr; }
QSGNode* HeatmapLayer::createContainer() {
    gate_ = new QSGOpacityNode();
    tile_ = new heatmap::gpu::HeatmapTileNode(tileStats_);
    gate_->appendChildNode(tile_);
    return gate_;
}
void HeatmapLayer::updateNodes(const ChartFrame& sync, LayerNodeContext&) {
    const auto& frame = sync.context;
    const auto& cam = frame.raster;
    heatmap::gpu::HeatmapTileNode::Frame tileFrame;
    std::optional<heatmap::gpu::ViewWindow> binView;
    if (sync.binView) binView = heatmap::gpu::ViewWindow{sync.binView->timeLo, sync.binView->timeHi,
                                                      sync.binView->priceLo, sync.binView->priceHi};
    prepared_ = prepareHeatmap(sync.drawHeatmap, cam.valid,
        [&](auto& target) {
            return gpu->prepareFrame(target, frame.surfaceBounds,
                {cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice, cam.drawnMaxPrice},
                sync.coverageGamma, binView);
        }, *gate_, *tile_, tileFrame);
}
void HeatmapLayer::afterRendering() {
    finishHeatmapFrame(tileStats_, m_gpuLabelsIncomplete,
        [this] { if (queueUpdate) queueUpdate(); },
        [this] { if (queueLabelCheck) queueLabelCheck(); });
}
bool HeatmapLayer::labelsChangedSinceDraw() const {
    return gpu && gpu->labelSignature() != m_gpuLabelSignature.load();
}

void HeatmapLayer::submitLabels(const ChartFrame& snapshot, LayerNodeContext& context) {
    const auto& frame = snapshot.context;
    const bool prepared = prepared_;
    if (m_heatmapLabelGlyphs.capacity() < heatmap::gpu::HeatmapLabelLayout::kMaxGlyphs)
        m_heatmapLabelGlyphs.reserve(heatmap::gpu::HeatmapLabelLayout::kMaxGlyphs);
    const auto labels = prepared && context.atlasReady && context.window ? gpu->labelsForFrame() : nullptr;
    m_gpuLabelSerial = labels ? labels->key.serial : 0;
    if (!labels) {
        m_heatmapLabelGlyphs.clear();
        m_gpuLabelSignature.store(0);
        // Held back by a transition: after the frame, check whether they can draw.
        m_gpuLabelsIncomplete.store(prepared && gpu->labelsPending());
        return;
    }
    // Only the columns showing the picture the labels describe (drawn last frame
    // and this frame's target: the node prepares after this layout).
    if (m_gpuLabelColumns.capacity() < heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells)
        m_gpuLabelColumns.reserve(heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells);
    const size_t matched = gpu->matchLabelColumns(*labels, m_gpuLabelColumns);
    const auto& settings = gpu->settings();
    heatmap::gpu::LabelStyle style;
    style.usd = settings.labelCurrency != "asset";
    style.minPx = settings.labelMinPx;
    style.maxPx = settings.labelMaxPx;
    style.window = {gpu->drawStyle().codeFloor, gpu->drawStyle().codeRange};
    style.palette = gpu->palette();
    m_gpuLabels.layout(labels, context.atlas, frame.mapping, frame.surfaceDpr, style,
                       m_heatmapLabelGlyphs, &m_gpuLabelColumns);
    const auto& st = m_gpuLabels.stats();
    m_gpuLabelSignature.store(heatmap::gpu::HeatmapGpuLayer::labelSignature(labels.get(), m_gpuLabelColumns, matched));
    m_gpuLabelsIncomplete.store(st.unmatched > 0);
    sLog_Probe("heatmap.labels.layout", "serial=" << labels->key.serial << " labels=" << st.labels
               << " glyphs=" << st.glyphs << " sizePx=" << st.sizePx << " narrow=" << st.tooNarrow
               << " dropped=" << st.droppedBudget << " unmatched=" << st.unmatched << " matchedColumns=" << matched);
    context.text.submitGlyphs(m_heatmapLabelGlyphs, ChartTextRenderer::Priority::Low);
}
} // namespace chart
