#pragma once
// QRhi resources for GPU price binning of a composed heatmap source.
// Render-thread only. All recording (upload, bin, readBack, updateDraw) happens
// outside a render pass, recordDraw inside one; HeatmapRenderNode does the former
// in QSGRenderNode::prepare() and the latter in render().
//
// Lifetime rules:
// - setSource() starts paging a new source into a FRESH buffer set; the active
//   source keeps binning/drawing until the new one is fully uploaded, then swaps.
// - The output grid buffer only grows; a resize never touches source buffers.
#include "HeatmapBinGrid.hpp"
#include "HeatmapGpuSource.hpp"
#include <QMatrix4x4>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiComputePipeline;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiShaderResourceBindings;
struct QRhiReadbackResult;

namespace heatmap::gpu {

enum class CellState : uint32_t { NoData = 0, Loading = 1, Veil = 2, Valid = 3 };
// Decoded output cell (bits 0..14 code, bit 15 ask, bits 16..17 state).
struct Cell {
    uint16_t code = 0;
    bool ask = false;
    CellState state = CellState::NoData;
};
inline Cell decodeCell(uint32_t word) {
    return {uint16_t(word & 0x7fffu), (word & 0x8000u) != 0, CellState((word >> 16) & 3u)};
}

struct DrawStyle {
    float codeFloor = 6000, codeRange = 24000; // recording-mode palette normalization
};

class HeatmapGpuBinner {
public:
    explicit HeatmapGpuBinner(QRhi *rhi);
    ~HeatmapGpuBinner();
    HeatmapGpuBinner(const HeatmapGpuBinner &) = delete;
    HeatmapGpuBinner &operator=(const HeatmapGpuBinner &) = delete;

    QRhi *rhi() const { return rhi_; }
    // No-op if `source` is already active or pending. nullptr clears nothing:
    // the active picture stays until a real replacement is uploaded.
    bool setSource(std::shared_ptr<const GpuSource> source, QString *error);
    // Records up to budgetBytes of the pending upload. Swaps it in when complete.
    bool uploadStep(QRhiCommandBuffer *cb, uint64_t budgetBytes, QString *error);
    // Convenience for headless tools: upload everything in one frame.
    bool uploadAll(QRhiCommandBuffer *cb, QString *error);
    bool uploadPending() const { return pending_ != nullptr; }
    std::shared_ptr<const GpuSource> activeSource() const;
    std::shared_ptr<const GpuSource> pendingSource() const;

    // One compute pass over the active source. Records outside a render pass.
    bool bin(QRhiCommandBuffer *cb, const BinGrid &grid, const recording::SizeScale &outputScale,
             QString *error);
    const std::optional<BinGrid> &binnedGrid() const { return binnedGrid_; }
    uint64_t binnedSourceId() const { return binnedSourceId_; }
    // Queues a readback of the binned grid: columns * rows uint32 cells, row 0 = top.
    bool readBack(QRhiCommandBuffer *cb, QRhiReadbackResult *result, QString *error);

    bool prepareDraw(QRhiRenderPassDescriptor *pass, int sampleCount, QString *error);
    void updateDraw(QRhiResourceUpdateBatch *updates, const QMatrix4x4 &mvp, const QRectF &itemRect,
                    const DisplayMapping &mapping, const DrawStyle &style = {});
    // Inside a render pass; the caller sets viewport/scissor.
    void recordDraw(QRhiCommandBuffer *cb);
    bool canDraw() const { return binnedGrid_.has_value() && graphics_ && drawBindings_; }

    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const;

private:
    struct SourceBuffers;
    QRhi *rhi_ = nullptr;
    std::unique_ptr<SourceBuffers> active_, pending_;
    std::unique_ptr<QRhiBuffer> thresholds_, output_, computeParams_, drawParams_;
    recording::SizeScale thresholdScale_{0, 0};
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::unique_ptr<QRhiComputePipeline> compute_;
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
    QVector<quint32> graphicsFormat_;
    int graphicsSamples_ = 0;
    std::optional<BinGrid> binnedGrid_;
    uint64_t binnedSourceId_ = 0;
    const SourceBuffers *computeBoundTo_ = nullptr;
    bool rebuildComputeBindings(QString *error);
    bool rebuildDrawBindings(QString *error);
};
} // namespace heatmap::gpu
