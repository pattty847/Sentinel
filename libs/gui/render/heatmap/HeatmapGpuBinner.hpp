#pragma once
// QRhi resources for GPU price binning of composed heatmap sources.
// Render-thread only. All recording (upload, bin, readBack) happens outside a
// render pass (QSGRenderNode::prepare()). Drawing is the caller's (HeatmapTileNode
// draws the cells it bins into its own buffers).
//
// Two ways to hold sources:
// - The resident pool (HeatmapTileNode): any number of span sources stay uploaded
//   side by side, paged in within a byte budget; binResidentInto() bins one into a
//   caller buffer, optionally as a fill pass over a coarser source's cells. Once a
//   resident upload completes the binner keeps only the source's bin parameters,
//   so the caller may release the CPU image.
// - One active source (self-test, parity tests, the compute benchmark): two
//   grow-only buffer sets; the active one binds, the spare one receives the (at
//   most one) pending source. New capacity is created one buffer (<= 64 MiB) per
//   upload step, then data pages in within the byte budget. The active source
//   keeps binning until the pending one is complete; a failed allocation drops
//   only the pending source.
// - Re-requesting the active source cancels a pending upload (A -> B -> A).
// - The output grid buffer only grows; a resize never touches source buffers.
// - Entries are split into <= 64 MiB pages (D3D11 guarantees 128 MB per buffer).
// - QRhi lifetime: the binner registers a keyed cleanup callback on its QRhi. If
//   the QRhi is destroyed first (scene graph invalidation, backends that skip
//   QSGRenderNode::releaseResources), the callback completes any in-flight
//   readback and releases every resource while the QRhi still works; the binner
//   is then inert (rhi() == nullptr) and its destructor never calls into it.
//   A binner destroyed while its QRhi lives completes an in-flight readback
//   first (rhi->finish()), because QRhi writes into the result when the frame
//   completes (FM-099).
// - Readbacks: the binner owns the QRhiReadbackResult; callers copy the data.
// Precision: once an active source exists, the first frame per QRhi
// backend/device runs a precision self-test (HeatmapGpuSelfTest; its CPU fixture
// is built on a worker, so the render thread only records the dispatch and the
// readback). Until it resolves, bins use the `precise` kernel; if it fails, the
// precise kernel stays (and is logged).
#include "HeatmapBinGrid.hpp"
#include "HeatmapGpuSource.hpp"
#include <QMatrix4x4>
#include <QRectF>
#include <QString>
#include <QVector>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>
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
    float opacity = 1;                         // premultiplied: scales colour and alpha
};

enum class KernelVariant { Fast, Precise };

class HeatmapGpuBinner {
public:
    static constexpr uint64_t kDefaultMemoryCapBytes = 512ull << 20;
    // memoryCapBytes bounds both source buffer sets together (active + spare).
    explicit HeatmapGpuBinner(QRhi *rhi, uint64_t memoryCapBytes = kDefaultMemoryCapBytes);
    ~HeatmapGpuBinner();
    HeatmapGpuBinner(const HeatmapGpuBinner &) = delete;
    HeatmapGpuBinner &operator=(const HeatmapGpuBinner &) = delete;

    // nullptr once the QRhi was destroyed (the binner is then inert).
    QRhi *rhi() const { return rhi_; }
    // No-op if `source` is already pending; cancels the pending upload if it is
    // the active source; otherwise replaces any pending source. nullptr is a no-op.
    // Never disturbs the active source; returns false (pending dropped) when it
    // refuses a source (limits, memory cap). A refused source is reported once:
    // requesting it again is a silent no-op, forever for limits/cap and until
    // the retry backoff elapses after an allocation failure (2 s doubling to 60 s).
    bool setSource(std::shared_ptr<const GpuSource> source, QString *error);
    // Creates at most one missing buffer, then records up to budgetBytes of data.
    // Swaps the pending source in when complete. Errors drop only the pending source.
    bool uploadStep(QRhiCommandBuffer *cb, uint64_t budgetBytes, QString *error);
    // Headless tools: allocate and upload everything in one frame.
    bool uploadAll(QRhiCommandBuffer *cb, QString *error);
    bool uploadPending() const { return pending_; }
    std::shared_ptr<const GpuSource> activeSource() const;
    std::shared_ptr<const GpuSource> pendingSource() const;

    // One compute pass over the active source into the binner's own output
    // (readBack()). Records outside a render pass.
    bool bin(QRhiCommandBuffer *cb, const BinGrid &grid, const recording::SizeScale &outputScale, QString *error);
    // Resident source pool: any number of sources stay uploaded side by side,
    // independent of the active/spare pair. Pages `source` in within `budget`
    // (decremented); *complete once it is fully uploaded, and from then on the
    // binner holds no reference to the CPU image. Buffers are created exactly
    // sized at the first call (a span source is a few MiB).
    bool uploadResident(const std::shared_ptr<const GpuSource> &source, QRhiCommandBuffer *cb, uint64_t &budget,
                        bool *complete, QString *error);
    bool isResident(uint64_t sourceId) const;
    // One compute pass over a resident source into a caller-owned storage buffer
    // of at least columns * rows * 4 bytes (the kernel's cell encoding); several
    // may be recorded in one frame. fill: a fill pass (heatmap_bin.comp dims.w
    // bit 1) that replaces only the target's veil cells, and only with valid ones.
    bool binResidentInto(uint64_t sourceId, QRhiCommandBuffer *cb, const BinGrid &grid,
                         const recording::SizeScale &outputScale, QRhiBuffer *target, QString *error,
                         bool fill = false);
    // Its buffers go through QRhiResource::deleteLater(): a frame being recorded
    // may still reference them (uploads, bins), so they die at its end.
    void releaseResident(uint64_t sourceId);
    uint64_t residentBytes() const;
    uint64_t residentBytes(uint64_t sourceId) const; // one source's buffers (0 if absent)
    const std::optional<BinGrid> &binnedGrid() const { return binnedGrid_; }
    uint64_t binnedSourceId() const { return binnedSourceId_; }
    // True if the last bin() used this source and output scale and the kernel
    // that would be used now (a resolved self-test switches kernels: re-bin).
    bool binnedMatches(uint64_t sourceId, const recording::SizeScale &outputScale) const;
    // Queues a readback of the binned grid (columns * rows uint32 cells, row 0 =
    // top) into a binner-owned result; one at a time (refused while in flight).
    // It completes when the recording frame completes (offscreen: at
    // endOffscreenFrame), then readBackData() returns a copy.
    bool readBack(QRhiCommandBuffer *cb, QString *error);
    bool readBackPending() const { return readbackPending_; }
    QByteArray readBackData() const; // empty while pending or before any readback

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
    // Marks the current thread as inside a QRhi cleanup-callback traversal (the
    // QRhi is being destroyed). Code that runs there must not destroy objects
    // that own a cleanup callback: their destructor would remove a callback from
    // the hash the QRhi is iterating. Tests count such removals.
    struct RhiCleanupScope {
        RhiCleanupScope();
        ~RhiCleanupScope();
    };
    static uint64_t callbackRemovalsDuringCleanupForTest();
    // Tests: a self-test run is recorded and not yet consumed; and how many
    // in-flight readbacks a destructor or QRhi cleanup completed before freeing them.
    bool selfTestInFlightForTest() const;
    static uint64_t drainedReadbacksForTest();
    // Starts (first call) or polls the precision self-test. bin() also calls it;
    // call it every frame so a static view still resolves to the fast kernel.
    void runPrecisionSelfTest(QRhiCommandBuffer *cb) { driveSelfTest(cb); }

    void setMemoryCap(uint64_t bytes) { if (bytes != memoryCapBytes_) { memoryCapBytes_ = bytes; refused_.reset(); } }
    uint64_t memoryCap() const { return memoryCapBytes_; }
    // Id of the source currently refused or backing off (0 if none).
    uint64_t refusedSourceId() const { return refused_ ? refused_->sourceId : 0; }
    std::chrono::milliseconds retryBackoff() const { return refused_ ? refused_->backoff : std::chrono::milliseconds(0); }
    void setAllocationFailureForTest(bool fail) { failAllocationsForTest_ = fail; }
    void setInitialRetryBackoffForTest(std::chrono::milliseconds backoff) { initialRetryBackoff_ = backoff; }

    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const;

private:
    struct SourceBuffers;
    struct SelfTestRun;
    std::unique_ptr<QRhiReadbackResult> readback_;
    bool readbackPending_ = false;
    void releaseForDeadRhi(); // QRhi cleanup callback
    QRhi *rhi_ = nullptr;
    std::unique_ptr<SourceBuffers> active_, spare_;
    std::unordered_map<uint64_t, std::unique_ptr<SourceBuffers>> resident_; // B1 hybrid pool, by source id
    bool pending_ = false;
    std::unique_ptr<QRhiBuffer> thresholds_, output_, computeParams_, dummy_;
    recording::SizeScale thresholdScale_{0, 0};
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_;
    // Pipelines are created against this layout template (same binding points,
    // types and stages, backed only by binner-owned buffers), which lives as long
    // as the pipelines. computeBindings_ is replaced per source, and Vulkan
    // invalidates a pipeline whose creation-time layout is gone.
    std::unique_ptr<QRhiBuffer> layoutUniform_;
    std::unique_ptr<QRhiShaderResourceBindings> computeLayout_;
    std::array<std::unique_ptr<QRhiComputePipeline>, 2> compute_; // Fast, Precise
    std::optional<BinGrid> binnedGrid_;
    uint64_t binnedSourceId_ = 0;
    recording::SizeScale binnedScale_{0, 0};
    KernelVariant binnedKernel_ = KernelVariant::Precise;
    const SourceBuffers *computeBoundTo_ = nullptr;
    std::optional<KernelVariant> resolved_;
    bool forced_ = false, testShader_ = false;
    QString fastShaderPath_ = QStringLiteral(":/heatmapgpu/heatmap_bin.comp.qsb");
    std::unique_ptr<SelfTestRun> selfTest_;
    uint64_t memoryCapBytes_ = kDefaultMemoryCapBytes;
    struct Refusal {
        uint64_t sourceId = 0;
        bool retryable = false; // allocation failure: retry after backoff
        std::chrono::steady_clock::time_point retryAt;
        std::chrono::milliseconds backoff{0};
    };
    std::optional<Refusal> refused_;
    static constexpr std::chrono::milliseconds kMaxRetryBackoff{60'000};
    std::chrono::milliseconds initialRetryBackoff_{2'000};
    bool failAllocationsForTest_ = false;
    bool rebuildComputeBindings(QString *error);
    std::unique_ptr<QRhiShaderResourceBindings> makeComputeBindings(const SourceBuffers &source, QRhiBuffer *output,
                                                                    QRhiBuffer *params, QString *error);
    bool binSourceInto(const SourceBuffers &source, QRhiCommandBuffer *cb, const BinGrid &grid,
                       const recording::SizeScale &outputScale, QRhiBuffer *target, bool fill, QString *error);
    bool ensureThresholds(QRhiResourceUpdateBatch *updates, const recording::SizeScale &outputScale, QString *error);
    // sourceParams: the binner's SourceParams (HeatmapGpuBinner.cpp).
    bool fillComputeParams(const void *sourceParams, const BinGrid &grid, const recording::SizeScale &outputScale,
                           bool fill, void *params, QString *error) const;
    bool ensureLayouts(QString *error);
    bool ensurePipeline(KernelVariant variant, QString *error);
    void driveSelfTest(QRhiCommandBuffer *cb);
    QString deviceKey() const;
};
} // namespace heatmap::gpu
