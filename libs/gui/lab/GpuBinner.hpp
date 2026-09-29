#pragma once
#include "servermodel/RecordingEntries.hpp"
#include <array>
#include <memory>
#include <vector>
#include <QString>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiShaderResourceBindings;
class QRhiComputePipeline;
class QRhiGraphicsPipeline;
class QRhiRenderTarget;
struct QRhiReadbackResult;

namespace lab {
struct Grid {
    float timeLo = 0, timeHi = 1;
    int32_t rowLo = 0;
    int32_t baseRow = 0; // absolute common-grid row used by price LOD indices
    uint32_t group = 1, columns = 1, rows = 1;
    float sizeFloor = 1e-6f, codesPerOctave = 819;
};

// Resources are owned on one QRhi/render thread. Source buffers are uploaded
// once per dataset; view changes only update a 48-byte uniform and output grid.
class GpuBinner {
public:
    explicit GpuBinner(QRhi *rhi);
    ~GpuBinner();
    GpuBinner(const GpuBinner &) = delete;
    GpuBinner &operator=(const GpuBinner &) = delete;
    bool upload(QRhiCommandBuffer *cb, const recording::RecordingEntries &data, QString *error);
    bool beginUpload(const recording::RecordingEntries &data, QString *error);
    bool uploadStep(QRhiCommandBuffer *cb, uint32_t budgetBytes, QString *error);
    bool uploadComplete() const { return uploadPart_ >= uploadParts_.size(); }
    bool bin(QRhiCommandBuffer *cb, const Grid &grid, QString *error);
    bool readBack(QRhiCommandBuffer *cb, const Grid &grid, QRhiReadbackResult *result, QString *error);
    bool draw(QRhiCommandBuffer *cb, QRhiRenderTarget *target, QString *error);
    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const { return bytes_; }
private:
    QRhi *rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> packed_, offsets_, coverage_, observed_, nativeFactor_;
    std::unique_ptr<QRhiBuffer> lodRowSide_, lodWeighted_, lodOffsets_, lodCoverage_, lodObserved_;
    std::unique_ptr<QRhiBuffer> priceRowSide_, priceSize_, priceOffsets_;
    std::unique_ptr<QRhiBuffer> timePriceRowSide_, timePriceSize_, timePriceOffsets_;
    std::array<std::unique_ptr<QRhiBuffer>, 12> dense_;
    std::unique_ptr<QRhiBuffer> output_, params_;
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::unique_ptr<QRhiComputePipeline> compute_;
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
    QRhiRenderTarget *graphicsTarget_ = nullptr;
    uint32_t sourceColumns_ = 0;
    uint64_t bytes_ = 0;
    struct UploadPart { QRhiBuffer *buffer; const void *source; uint32_t bytes; bool packed; };
    struct alignas(16) ColumnGpuMeta { uint32_t factor; float floor; float codesPerOctave; uint32_t reserved; };
    std::vector<UploadPart> uploadParts_;
    std::vector<ColumnGpuMeta> columnMetaStaging_;
    const recording::RecordingEntries *uploadData_ = nullptr;
    size_t uploadPart_ = 0;
    uint32_t uploadOffset_ = 0;
    bool makeBindings(QString *error);
};
} // namespace lab
