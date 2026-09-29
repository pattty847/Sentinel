#pragma once
#include "servermodel/RecordingEntries.hpp"
#include <array>
#include <memory>
#include <QString>

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiShaderResourceBindings;
class QRhiComputePipeline;
class QRhiGraphicsPipeline;
class QRhiRenderTarget;

namespace lab {
struct Grid {
    float timeLo = 0, timeHi = 1;
    int32_t rowLo = 0;
    int32_t rowAlignment = 0; // baseRow modulo group for globally aligned price LOD
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
    bool bin(QRhiCommandBuffer *cb, const Grid &grid, QString *error);
    bool draw(QRhiCommandBuffer *cb, QRhiRenderTarget *target, QString *error);
    uint64_t gpuBytes() const;
    uint64_t sourceBytes() const { return bytes_; }
private:
    QRhi *rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> packed_, offsets_, coverage_, observed_;
    std::unique_ptr<QRhiBuffer> lodRowSide_, lodWeighted_, lodOffsets_, lodCoverage_, lodObserved_;
    std::unique_ptr<QRhiBuffer> priceRowSide_, priceSize_, priceOffsets_;
    std::unique_ptr<QRhiBuffer> timePriceRowSide_, timePriceSize_, timePriceOffsets_;
    std::array<std::unique_ptr<QRhiBuffer>, 8> dense_;
    std::unique_ptr<QRhiBuffer> output_, params_;
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::unique_ptr<QRhiComputePipeline> compute_;
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
    QRhiRenderTarget *graphicsTarget_ = nullptr;
    uint32_t sourceColumns_ = 0;
    uint64_t bytes_ = 0;
    bool makeBindings(QString *error);
};
} // namespace lab
