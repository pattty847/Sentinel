#pragma once
#include "servermodel/RecordingEntries.hpp"
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
// Each x is one UTC epoch aligned timeframe; each y is one absolute price bin.
struct Grid {
    int32_t firstMinute = 0, rowLo = 0, baseRow = 0;
    uint32_t timeframeMinutes = 1, group = 1, columns = 1, rows = 1;
    float sizeFloor = 1e-6f, codesPerOctave = 819;
};
struct DisplayMapping {
    float timeOffset = 0, timeSpan = 1;   // from grid left, in columns
    float priceOffset = 0, priceSpan = 1; // from grid top, in rows
};
// A pan inside this guard changes only DisplayMapping, so bin boundaries and
// values remain fixed while the fragment pass translates them continuously.
bool gridContainsView(const Grid &grid, int64_t sourceStartMinute, int64_t baseRow,
                      double absoluteMinuteLo, double absoluteMinuteHi,
                      double priceLo, double priceHi, double displayTick);

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
    bool draw(QRhiCommandBuffer *cb, QRhiRenderTarget *target, const DisplayMapping &mapping, QString *error);
    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const { return bytes_; }
private:
    QRhi *rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> packed_, offsets_, coverage_, observed_, nativeFactor_;
    std::unique_ptr<QRhiBuffer> entryCovered_, coverageRuns_, runOffsets_;
    std::unique_ptr<QRhiBuffer> output_, computeParams_, drawParams_;
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::unique_ptr<QRhiComputePipeline> compute_;
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
    QRhiRenderTarget *graphicsTarget_ = nullptr;
    uint32_t sourceColumns_ = 0, outputColumns_ = 0, outputRows_ = 0;
    uint32_t sourceMinutes_ = 1;
    bool preNormalized_ = false;
    bool compactFour_ = false;
    float sizeFloor_ = 1e-6f, codesPerOctave_ = 819;
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
