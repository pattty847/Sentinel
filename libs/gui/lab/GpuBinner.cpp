#include "GpuBinner.hpp"
#include <QColor>
#include <QFile>
#include <rhi/qshader.h>
#include <rhi/qrhi.h>
#include <algorithm>
#include <array>
#include <limits>

namespace lab {
bool gridContainsView(const Grid &grid, int64_t sourceStartMinute, int64_t baseRow,
                      double absoluteMinuteLo, double absoluteMinuteHi,
                      double priceLo, double priceHi, double displayTick) {
    if (!grid.timeframeMinutes || !grid.group || !(displayTick > 0)) return false;
    const int64_t timeFirst = (sourceStartMinute + grid.firstMinute) / grid.timeframeMinutes;
    const int64_t priceFirst = (baseRow + grid.rowLo) / int64_t(grid.group);
    return int64_t(std::floor(absoluteMinuteLo / grid.timeframeMinutes)) >= timeFirst + 1 &&
           int64_t(std::ceil(absoluteMinuteHi / grid.timeframeMinutes)) <=
               timeFirst + grid.columns - 1 &&
           int64_t(std::floor(priceLo / displayTick)) >= priceFirst + 1 &&
           int64_t(std::ceil(priceHi / displayTick)) <= priceFirst + grid.rows - 1;
}
namespace {
QShader shader(const char *path) {
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(file.readAll());
}
template <class T> bool make(QRhi *rhi, std::unique_ptr<QRhiBuffer> &dst,
                             QRhiBuffer::Type type, QRhiBuffer::UsageFlags flags,
                             size_t count, QString *error) {
    const uint64_t bytes = std::max<size_t>(count * sizeof(T), 16);
    if (bytes > std::numeric_limits<uint32_t>::max()) {
        if (error) *error = QStringLiteral("single GPU buffer exceeds 4 GiB");
        return false;
    }
    dst.reset(rhi->newBuffer(type, flags, static_cast<uint32_t>(bytes)));
    if (!dst || !dst->create()) {
        if (error) *error = QStringLiteral("GPU buffer allocation failed (%1 bytes)").arg(bytes);
        return false;
    }
    return true;
}
struct alignas(16) ComputeParams {
    std::array<uint32_t, 4> dims;
    std::array<int32_t, 4> coords;
    std::array<float, 4> scale;
};
struct alignas(16) DrawParams {
    std::array<uint32_t, 4> dims;
    std::array<float, 4> mapping;
    std::array<float, 4> color;
};
static_assert(sizeof(ComputeParams) == 48 && sizeof(DrawParams) == 48);
} // namespace

GpuBinner::GpuBinner(QRhi *rhi) : rhi_(rhi) {}
GpuBinner::~GpuBinner() = default;
uint64_t GpuBinner::gpuBytes() const {
    return bytes_ + (output_ ? output_->size() : 0) +
           (computeParams_ ? computeParams_->size() : 0) + (drawParams_ ? drawParams_->size() : 0);
}

bool GpuBinner::upload(QRhiCommandBuffer *cb, const recording::RecordingEntries &data, QString *error) {
    if (!beginUpload(data, error)) return false;
    while (!uploadComplete())
        if (!uploadStep(cb, 8 * 1024 * 1024, error)) return false;
    return true;
}

bool GpuBinner::beginUpload(const recording::RecordingEntries &data, QString *error) {
    compute_.reset(); graphics_.reset(); computeBindings_.reset(); drawBindings_.reset();
    packed_.reset(); offsets_.reset(); coverage_.reset(); observed_.reset(); nativeFactor_.reset();
    entryCovered_.reset(); coverageRuns_.reset(); runOffsets_.reset();
    output_.reset(); computeParams_.reset(); drawParams_.reset();
    outputColumns_ = outputRows_ = 0;
    sourceColumns_ = data.columns(); sourceMinutes_ = data.sourceMinutes;
    preNormalized_ = data.preNormalized; bytes_ = 0;
    compactFour_ = std::all_of(data.rowSide.begin(), data.rowSide.end(),
        [](uint32_t value) { return (value & 0x7fffffffu) <= 0xffffu; });
    uploadParts_.clear(); uploadPart_ = 0; uploadOffset_ = 0; uploadData_ = nullptr;
    if (!sourceColumns_ || data.rowSide.size() != data.code.size() ||
        data.offsets.size() != size_t(sourceColumns_) + 1 || data.coverage.size() != sourceColumns_ ||
        data.observedMs.size() != sourceColumns_ || data.nativeFactor.size() != sourceColumns_ ||
        data.columnScale.size() != sourceColumns_ ||
        (sourceMinutes_ != 1 && (data.entryCoveredMs.size() != data.rowSide.size() ||
          data.coverageRunOffsets.size() != size_t(sourceColumns_) * 2 + 1))) {
        if (error) *error = QStringLiteral("invalid sparse entry arrays");
        return false;
    }
    const auto storage = QRhiBuffer::StorageBuffer;
    const size_t packedWords = compactFour_ ? data.rowSide.size() :
                               ((data.rowSide.size() + 1) / 2) * 3;
    if (!make<uint32_t>(rhi_, packed_, QRhiBuffer::Static, storage, packedWords, error) ||
        !make<uint32_t>(rhi_, offsets_, QRhiBuffer::Static, storage, data.offsets.size(), error) ||
        !make<recording::RecordingEntries::Coverage>(rhi_, coverage_, QRhiBuffer::Static, storage, data.coverage.size(), error) ||
        !make<uint32_t>(rhi_, observed_, QRhiBuffer::Static, storage, data.observedMs.size(), error) ||
        !make<ColumnGpuMeta>(rhi_, nativeFactor_, QRhiBuffer::Static, storage, data.nativeFactor.size(), error) ||
        !make<uint32_t>(rhi_, entryCovered_, QRhiBuffer::Static, storage, data.entryCoveredMs.size(), error) ||
        !make<std::array<int32_t, 4>>(rhi_, coverageRuns_, QRhiBuffer::Static, storage, data.coverageRuns.size(), error) ||
        !make<uint32_t>(rhi_, runOffsets_, QRhiBuffer::Static, storage, data.coverageRunOffsets.size(), error) ||
        !make<ComputeParams>(rhi_, computeParams_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 1, error) ||
        !make<DrawParams>(rhi_, drawParams_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 1, error)) return false;
    auto add = [&](QRhiBuffer *buffer, const auto &values) {
        if (!values.empty())
            uploadParts_.push_back({buffer, values.data(), uint32_t(values.size() * sizeof(values[0])), false});
    };
    if (!data.rowSide.empty()) uploadParts_.push_back({packed_.get(), nullptr, uint32_t(packedWords * 4), true});
    add(offsets_.get(), data.offsets);
    add(coverage_.get(), data.coverage);
    add(observed_.get(), data.observedMs);
    columnMetaStaging_.resize(sourceColumns_);
    for (uint32_t c = 0; c < sourceColumns_; ++c)
        columnMetaStaging_[c] = {data.nativeFactor[c], float(data.columnScale[c].floor),
                                 float(data.columnScale[c].codesPerOctave), 0};
    add(nativeFactor_.get(), columnMetaStaging_);
    add(entryCovered_.get(), data.entryCoveredMs);
    add(coverageRuns_.get(), data.coverageRuns);
    add(runOffsets_.get(), data.coverageRunOffsets);
    bytes_ = packed_->size() + offsets_->size() + coverage_->size() + observed_->size() +
             nativeFactor_->size() + entryCovered_->size() + coverageRuns_->size() + runOffsets_->size();
    uploadData_ = &data;
    return true;
}

bool GpuBinner::uploadStep(QRhiCommandBuffer *cb, uint32_t budgetBytes, QString *error) {
    if (!uploadData_ || !cb || !budgetBytes) {
        if (error) *error = QStringLiteral("upload was not initialized");
        return false;
    }
    if (uploadComplete()) return true;
    auto *updates = rhi_->nextResourceUpdateBatch();
    uint32_t remaining = std::max<uint32_t>(budgetBytes, 12);
    while (uploadPart_ < uploadParts_.size() && remaining >= 12) {
        const auto &part = uploadParts_[uploadPart_];
        uint32_t chunk = std::min(part.bytes - uploadOffset_, remaining);
        chunk = part.packed && !compactFour_ ? (chunk / 12) * 12 : (chunk / 4) * 4;
        if (!chunk) break;
        if (part.packed) {
            std::vector<uint32_t> words(chunk / 4, 0);
            if (compactFour_) {
                const size_t firstEntry = uploadOffset_ / 4;
                for (size_t p = 0; p < words.size(); ++p) {
                    const size_t i = firstEntry + p;
                    words[p] = (uploadData_->rowSide[i] & 0xffffu) |
                               ((uploadData_->rowSide[i] >> 31) << 16) |
                               ((uploadData_->code[i] & 0x7fffu) << 17);
                }
            } else {
                const size_t firstPair = uploadOffset_ / 12;
                for (size_t p = 0; p < words.size() / 3; ++p) {
                    const size_t i = (firstPair + p) * 2;
                    words[p * 3] = uploadData_->rowSide[i];
                    words[p * 3 + 2] = uploadData_->code[i] & 0xffffu;
                    if (i + 1 < uploadData_->rowSide.size()) {
                        words[p * 3 + 1] = uploadData_->rowSide[i + 1];
                        words[p * 3 + 2] |= (uploadData_->code[i + 1] & 0xffffu) << 16;
                    }
                }
            }
            updates->uploadStaticBuffer(part.buffer, uploadOffset_, chunk, words.data());
        } else {
            updates->uploadStaticBuffer(part.buffer, uploadOffset_, chunk,
                                        static_cast<const char *>(part.source) + uploadOffset_);
        }
        uploadOffset_ += chunk;
        remaining -= chunk;
        if (uploadOffset_ == part.bytes) { ++uploadPart_; uploadOffset_ = 0; }
    }
    cb->resourceUpdate(updates);
    return true;
}

bool GpuBinner::makeBindings(QString *error) {
    computeBindings_.reset(rhi_->newShaderResourceBindings());
    const auto cs = QRhiShaderResourceBinding::ComputeStage;
    computeBindings_->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, cs, packed_.get()),
        QRhiShaderResourceBinding::bufferLoad(1, cs, offsets_.get()),
        QRhiShaderResourceBinding::bufferLoad(2, cs, coverage_.get()),
        QRhiShaderResourceBinding::bufferLoad(3, cs, observed_.get()),
        QRhiShaderResourceBinding::bufferLoad(4, cs, nativeFactor_.get()),
        QRhiShaderResourceBinding::bufferLoad(5, cs, entryCovered_.get()),
        QRhiShaderResourceBinding::bufferLoad(6, cs, coverageRuns_.get()),
        QRhiShaderResourceBinding::bufferLoad(7, cs, runOffsets_.get()),
        QRhiShaderResourceBinding::bufferStore(8, cs, output_.get()),
        QRhiShaderResourceBinding::uniformBuffer(9, cs, computeParams_.get())});
    if (!computeBindings_->create()) {
        if (error) *error = QStringLiteral("compute bindings failed");
        return false;
    }
    const auto qsb = shader(":/labshaders/bin.comp.qsb");
    if (!qsb.isValid()) { if (error) *error = QStringLiteral("compute shader missing"); return false; }
    compute_.reset(rhi_->newComputePipeline());
    compute_->setShaderStage({QRhiShaderStage::Compute, qsb});
    compute_->setShaderResourceBindings(computeBindings_.get());
    if (!compute_->create()) { if (error) *error = QStringLiteral("compute pipeline failed"); return false; }
    drawBindings_.reset(rhi_->newShaderResourceBindings());
    drawBindings_->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::FragmentStage, output_.get()),
        QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage, drawParams_.get())});
    if (!drawBindings_->create()) { if (error) *error = QStringLiteral("fragment bindings failed"); return false; }
    graphics_.reset(); graphicsTarget_ = nullptr;
    return true;
}

bool GpuBinner::bin(QRhiCommandBuffer *cb, const Grid &grid, QString *error) {
    if (!sourceColumns_ || !grid.columns || !grid.rows || !grid.group || !grid.timeframeMinutes ||
        grid.timeframeMinutes % sourceMinutes_ || grid.firstMinute % int32_t(sourceMinutes_) ||
        grid.columns > 16384 || grid.rows > 16384) {
        if (error) *error = QStringLiteral("invalid output grid"); return false;
    }
    const uint64_t cells = uint64_t(grid.columns) * grid.rows;
    if (!output_ || output_->size() < cells * 16) {
        compute_.reset(); graphics_.reset(); computeBindings_.reset(); drawBindings_.reset(); output_.reset();
        if (!make<std::array<float, 4>>(rhi_, output_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, cells, error) ||
            !makeBindings(error)) return false;
    }
    outputColumns_ = grid.columns; outputRows_ = grid.rows;
    sizeFloor_ = grid.sizeFloor; codesPerOctave_ = grid.codesPerOctave;
    ComputeParams params{{grid.columns, grid.rows, sourceColumns_, grid.group},
                         {grid.firstMinute / int32_t(sourceMinutes_),
                          int32_t(grid.timeframeMinutes / sourceMinutes_), grid.rowLo, grid.baseRow},
                         {compactFour_ ? 1.0f : 0.0f, grid.codesPerOctave, float(sourceMinutes_),
                          preNormalized_ ? 1.0f : 0.0f}};
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(computeParams_.get(), 0, sizeof(params), &params);
    cb->beginComputePass(updates);
    cb->setComputePipeline(compute_.get());
    cb->setShaderResources(computeBindings_.get());
    cb->dispatch(int((grid.columns + 7) / 8), int((grid.rows + 7) / 8), 1);
    cb->endComputePass();
    return true;
}

bool GpuBinner::readBack(QRhiCommandBuffer *cb, const Grid &grid, QRhiReadbackResult *result, QString *error) {
    const uint64_t bytes = uint64_t(grid.columns) * grid.rows * 16;
    if (!output_ || !result || bytes > output_->size() || bytes > std::numeric_limits<quint32>::max()) {
        if (error) *error = QStringLiteral("invalid readback grid"); return false;
    }
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->readBackBuffer(output_.get(), 0, quint32(bytes), result);
    cb->resourceUpdate(updates);
    return true;
}

bool GpuBinner::draw(QRhiCommandBuffer *cb, QRhiRenderTarget *target,
                     const DisplayMapping &mapping, QString *error) {
    if (!drawBindings_) { if (error) *error = QStringLiteral("bin first"); return false; }
    if (!graphics_ || graphicsTarget_ != target) {
        const auto vs = shader(":/labshaders/display.vert.qsb");
        const auto fs = shader(":/labshaders/display.frag.qsb");
        if (!vs.isValid() || !fs.isValid()) {
            if (error) *error = QStringLiteral("display shaders missing"); return false;
        }
        graphics_.reset(rhi_->newGraphicsPipeline());
        graphics_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        graphics_->setShaderResourceBindings(drawBindings_.get());
        graphics_->setRenderPassDescriptor(target->renderPassDescriptor());
        graphics_->setTopology(QRhiGraphicsPipeline::Triangles);
        if (!graphics_->create()) { if (error) *error = QStringLiteral("display pipeline failed"); return false; }
        graphicsTarget_ = target;
    }
    const DrawParams params{{outputColumns_, outputRows_, 0, 0},
                            {mapping.timeOffset, mapping.timeSpan, mapping.priceOffset, mapping.priceSpan},
                            {sizeFloor_, codesPerOctave_, 0, 0}};
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(drawParams_.get(), 0, sizeof(params), &params);
    cb->resourceUpdate(updates);
    cb->beginPass(target, QColor(7, 10, 15, 255), {1.0f, 0});
    cb->setGraphicsPipeline(graphics_.get());
    cb->setShaderResources(drawBindings_.get());
    cb->setViewport(QRhiViewport(0, 0, target->pixelSize().width(), target->pixelSize().height()));
    cb->draw(3);
    cb->endPass();
    return true;
}
} // namespace lab
