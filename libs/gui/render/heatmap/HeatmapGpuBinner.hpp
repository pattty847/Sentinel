#pragma once
// QRhi resources for GPU price binning of a composed heatmap source.
// Render-thread only. All recording (upload, bin, readBack, updateDraw) happens
// outside a render pass, recordDraw inside one; HeatmapRenderNode does the former
// in QSGRenderNode::prepare() and the latter in render().
//
// Lifetime rules:
// - Two grow-only buffer sets: the active one binds, the spare one receives the
//   (at most one) pending source. New capacity is created one buffer (<= 64 MiB)
//   per upload step, then data pages in within the byte budget. The active source
//   keeps binning and drawing until the pending one is complete; a failed
//   allocation drops only the pending source.
// - Re-requesting the active source cancels a pending upload (A -> B -> A).
// - The output grid buffer only grows; a resize never touches source buffers.
// - Entries are split into <= 64 MiB pages (D3D11 guarantees 128 MB per buffer).
// Precision: the first bin() per QRhi backend/device runs a precision self-test
// (HeatmapGpuSelfTest) with the fast kernel. Until it resolves, bins use the
// `precise` kernel; if it fails, the precise kernel stays (and is logged).
#include "HeatmapBinGrid.hpp"
#include "HeatmapGpuSource.hpp"
#include <QMatrix4x4>
#include <QRectF>
#include <QString>
#include <QVector>
#include <array>
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

enum class KernelVariant { Fast, Precise };

class HeatmapGpuBinner {
public:
    explicit HeatmapGpuBinner(QRhi *rhi);
    ~HeatmapGpuBinner();
    HeatmapGpuBinner(const HeatmapGpuBinner &) = delete;
    HeatmapGpuBinner &operator=(const HeatmapGpuBinner &) = delete;

    QRhi *rhi() const { return rhi_; }
    // No-op if `source` is already pending; cancels the pending upload if it is
    // the active source; otherwise replaces any pending source. nullptr is a no-op.
    // Never disturbs the active source; returns false (pending dropped) on error.
    bool setSource(std::shared_ptr<const GpuSource> source, QString *error);
    // Creates at most one missing buffer, then records up to budgetBytes of data.
    // Swaps the pending source in when complete. Errors drop only the pending source.
    bool uploadStep(QRhiCommandBuffer *cb, uint64_t budgetBytes, QString *error);
    // Headless tools: allocate and upload everything in one frame.
    bool uploadAll(QRhiCommandBuffer *cb, QString *error);
    bool uploadPending() const { return pending_; }
    std::shared_ptr<const GpuSource> activeSource() const;
    std::shared_ptr<const GpuSource> pendingSource() const;

    // One compute pass over the active source. Records outside a render pass.
    bool bin(QRhiCommandBuffer *cb, const BinGrid &grid, const recording::SizeScale &outputScale,
             QString *error);
    const std::optional<BinGrid> &binnedGrid() const { return binnedGrid_; }
    uint64_t binnedSourceId() const { return binnedSourceId_; }
    // True if the last bin() used this source and output scale and the kernel
    // that would be used now (a resolved self-test switches kernels: re-bin).
    bool binnedMatches(uint64_t sourceId, const recording::SizeScale &outputScale) const;
    // Queues a readback of the binned grid: columns * rows uint32 cells, row 0 = top.
    bool readBack(QRhiCommandBuffer *cb, QRhiReadbackResult *result, QString *error);

    bool prepareDraw(QRhiRenderPassDescriptor *pass, int sampleCount, QString *error);
    void updateDraw(QRhiResourceUpdateBatch *updates, const QMatrix4x4 &mvp, const QRectF &itemRect,
                    const DisplayMapping &mapping, const DrawStyle &style = {});
    // Inside a render pass; the caller sets viewport/scissor.
    void recordDraw(QRhiCommandBuffer *cb);
    bool canDraw() const { return binnedGrid_.has_value() && graphics_ && drawBindings_; }

    // Kernel in use for the next bin(); nullopt from resolvedKernel() while the
    // self-test is still in flight.
    KernelVariant currentKernel() const;
    std::optional<KernelVariant> resolvedKernel() const { return resolved_; }
    // Tests/benchmarks: pin a kernel (skips the self-test).
    void forceKernel(KernelVariant variant) { resolved_ = variant; forced_ = true; }
    // Tests: use this compiled shader as the "fast" kernel candidate; bypasses
    // the per-device self-test cache so the candidate is actually tested.
    void setFastKernelShaderForTest(const QString &qsbPath) { fastShaderPath_ = qsbPath; testShader_ = true; }
    static void clearSelfTestCacheForTest();
    // Starts (first call) or polls the precision self-test. bin() also calls it;
    // call it every frame so a static view still resolves to the fast kernel.
    void runPrecisionSelfTest(QRhiCommandBuffer *cb) { driveSelfTest(cb); }

    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const;

private:
    struct SourceBuffers;
    struct SelfTestRun;
    QRhi *rhi_ = nullptr;
    std::unique_ptr<SourceBuffers> active_, spare_;
    bool pending_ = false;
    std::unique_ptr<QRhiBuffer> thresholds_, output_, computeParams_, drawParams_, dummy_;
    recording::SizeScale thresholdScale_{0, 0};
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::array<std::unique_ptr<QRhiComputePipeline>, 2> compute_; // Fast, Precise
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
    QVector<quint32> graphicsFormat_;
    int graphicsSamples_ = 0;
    std::optional<BinGrid> binnedGrid_;
    uint64_t binnedSourceId_ = 0;
    recording::SizeScale binnedScale_{0, 0};
    KernelVariant binnedKernel_ = KernelVariant::Precise;
    const SourceBuffers *computeBoundTo_ = nullptr;
    std::optional<KernelVariant> resolved_;
    bool forced_ = false, testShader_ = false;
    QString fastShaderPath_ = QStringLiteral(":/heatmapgpu/heatmap_bin.comp.qsb");
    std::unique_ptr<SelfTestRun> selfTest_;
    bool rebuildComputeBindings(QString *error);
    bool rebuildDrawBindings(QString *error);
    bool ensurePipeline(KernelVariant variant, QString *error);
    void driveSelfTest(QRhiCommandBuffer *cb);
    QString deviceKey() const;
};
} // namespace heatmap::gpu
