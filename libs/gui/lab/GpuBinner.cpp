#include "GpuBinner.hpp"
#include <QFile>
#include <rhi/qshader.h>
#include <QColor>
#include <rhi/qrhi.h>
#include <array>
#include <limits>

namespace lab {
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
struct alignas(16) Params {
    std::array<uint32_t, 4> dims;
    std::array<float, 4> time;
    std::array<int32_t, 4> price;
};
static_assert(sizeof(Params) == 48);
} // namespace

GpuBinner::GpuBinner(QRhi *rhi) : rhi_(rhi) {}
GpuBinner::~GpuBinner() = default;
uint64_t GpuBinner::gpuBytes() const {
    return bytes_ + (output_ ? output_->size() : 0) + (params_ ? params_->size() : 0);
}

bool GpuBinner::upload(QRhiCommandBuffer *cb, const recording::RecordingEntries &data, QString *error) {
    compute_.reset(); graphics_.reset(); computeBindings_.reset(); drawBindings_.reset();
    packed_.reset(); offsets_.reset(); coverage_.reset(); observed_.reset();
    lodRowSide_.reset(); lodWeighted_.reset(); lodOffsets_.reset(); lodCoverage_.reset(); lodObserved_.reset();
    priceRowSide_.reset(); priceSize_.reset(); priceOffsets_.reset();
    timePriceRowSide_.reset(); timePriceSize_.reset(); timePriceOffsets_.reset();
    for (auto &buffer : dense_) buffer.reset();
    output_.reset(); params_.reset();
    sourceColumns_ = data.columns(); bytes_ = 0;
    if (!sourceColumns_ || data.rowSide.size() != data.code.size() ||
        data.offsets.size() != size_t(sourceColumns_) + 1 || data.coverage.size() != sourceColumns_ ||
        data.observedMs.size() != sourceColumns_ ||
        data.lod.rowSide.size() != data.lod.weightedSize.size() ||
        data.lod.offsets.size() != data.lod.coverage.size() + 1 ||
        data.lod.coverage.size() != data.lod.observedMs.size() ||
        data.priceLod.rowSide.size() != data.priceLod.size.size() ||
        data.priceLod.offsets.size() != size_t(sourceColumns_) + 1 ||
        data.timePriceLod.rowSide.size() != data.timePriceLod.size.size() ||
        data.timePriceLod.offsets.size() != data.lod.coverage.size() + 1) {
        if (error) *error = QStringLiteral("invalid sparse entry arrays");
        return false;
    }
    const auto storage = QRhiBuffer::StorageBuffer;
    const std::array<const recording::RecordingEntries::DensePriceLod *, 4> dense{
        &data.dense50, &data.dense100, &data.timeDense50, &data.timeDense100};
    for (size_t i = 0; i < dense.size(); ++i) {
        const size_t expected = i < 2 ? size_t(sourceColumns_) + 1 : data.lod.coverage.size() + 1;
        if (dense[i]->meta.size() != expected) {
            if (error) *error = QStringLiteral("invalid dense price LOD metadata"); return false;
        }
    }
    const size_t packedWords = ((data.rowSide.size() + 1) / 2) * 3;
    if (!make<uint32_t>(rhi_, packed_, QRhiBuffer::Static, storage, packedWords, error) ||
        !make<uint32_t>(rhi_, offsets_, QRhiBuffer::Static, storage, data.offsets.size(), error) ||
        !make<recording::RecordingEntries::Coverage>(rhi_, coverage_, QRhiBuffer::Static, storage, data.coverage.size(), error) ||
        !make<uint32_t>(rhi_, observed_, QRhiBuffer::Static, storage, data.observedMs.size(), error) ||
        !make<uint32_t>(rhi_, lodRowSide_, QRhiBuffer::Static, storage, data.lod.rowSide.size(), error) ||
        !make<float>(rhi_, lodWeighted_, QRhiBuffer::Static, storage, data.lod.weightedSize.size(), error) ||
        !make<uint32_t>(rhi_, lodOffsets_, QRhiBuffer::Static, storage, data.lod.offsets.size(), error) ||
        !make<recording::RecordingEntries::Coverage>(rhi_, lodCoverage_, QRhiBuffer::Static, storage, data.lod.coverage.size(), error) ||
        !make<uint32_t>(rhi_, lodObserved_, QRhiBuffer::Static, storage, data.lod.observedMs.size(), error) ||
        !make<uint32_t>(rhi_, priceRowSide_, QRhiBuffer::Static, storage, data.priceLod.rowSide.size(), error) ||
        !make<float>(rhi_, priceSize_, QRhiBuffer::Static, storage, data.priceLod.size.size(), error) ||
        !make<uint32_t>(rhi_, priceOffsets_, QRhiBuffer::Static, storage, data.priceLod.offsets.size(), error) ||
        !make<uint32_t>(rhi_, timePriceRowSide_, QRhiBuffer::Static, storage, data.timePriceLod.rowSide.size(), error) ||
        !make<float>(rhi_, timePriceSize_, QRhiBuffer::Static, storage, data.timePriceLod.size.size(), error) ||
        !make<uint32_t>(rhi_, timePriceOffsets_, QRhiBuffer::Static, storage, data.timePriceLod.offsets.size(), error) ||
        !make<Params>(rhi_, params_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 1, error)) return false;
    for (size_t i = 0; i < dense.size(); ++i) {
        if (!make<std::array<float, 2>>(rhi_, dense_[i * 2], QRhiBuffer::Static, storage,
                                         dense[i]->sums.size(), error) ||
            !make<std::array<int32_t, 2>>(rhi_, dense_[i * 2 + 1], QRhiBuffer::Static, storage,
                                           dense[i]->meta.size(), error)) return false;
    }
    auto *updates = rhi_->nextResourceUpdateBatch();
    if (!data.rowSide.empty()) {
        std::vector<uint32_t> packed(std::max<size_t>(packedWords, 4), 0);
        for (size_t i = 0; i < data.rowSide.size(); ++i) {
            const size_t word = (i / 2) * 3;
            packed[word + i % 2] = data.rowSide[i];
            packed[word + 2] |= (data.code[i] & 0xffffu) << ((i % 2) * 16);
        }
        updates->uploadStaticBuffer(packed_.get(), packed.data());
    }
    updates->uploadStaticBuffer(offsets_.get(), data.offsets.data());
    updates->uploadStaticBuffer(coverage_.get(), data.coverage.data());
    updates->uploadStaticBuffer(observed_.get(), data.observedMs.data());
    if (!data.lod.rowSide.empty()) {
        updates->uploadStaticBuffer(lodRowSide_.get(), data.lod.rowSide.data());
        updates->uploadStaticBuffer(lodWeighted_.get(), data.lod.weightedSize.data());
    }
    updates->uploadStaticBuffer(lodOffsets_.get(), data.lod.offsets.data());
    updates->uploadStaticBuffer(lodCoverage_.get(), data.lod.coverage.data());
    updates->uploadStaticBuffer(lodObserved_.get(), data.lod.observedMs.data());
    if (!data.priceLod.rowSide.empty()) {
        updates->uploadStaticBuffer(priceRowSide_.get(), data.priceLod.rowSide.data());
        updates->uploadStaticBuffer(priceSize_.get(), data.priceLod.size.data());
    }
    updates->uploadStaticBuffer(priceOffsets_.get(), data.priceLod.offsets.data());
    if (!data.timePriceLod.rowSide.empty()) {
        updates->uploadStaticBuffer(timePriceRowSide_.get(), data.timePriceLod.rowSide.data());
        updates->uploadStaticBuffer(timePriceSize_.get(), data.timePriceLod.size.data());
    }
    updates->uploadStaticBuffer(timePriceOffsets_.get(), data.timePriceLod.offsets.data());
    for (size_t i = 0; i < dense.size(); ++i) {
        if (!dense[i]->sums.empty()) updates->uploadStaticBuffer(dense_[i * 2].get(), dense[i]->sums.data());
        updates->uploadStaticBuffer(dense_[i * 2 + 1].get(), dense[i]->meta.data());
    }
    cb->resourceUpdate(updates);
    bytes_ = data.gpuBytes();
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
        QRhiShaderResourceBinding::bufferLoad(4, cs, lodRowSide_.get()),
        QRhiShaderResourceBinding::bufferLoad(5, cs, lodWeighted_.get()),
        QRhiShaderResourceBinding::bufferLoad(6, cs, lodOffsets_.get()),
        QRhiShaderResourceBinding::bufferLoad(7, cs, lodCoverage_.get()),
        QRhiShaderResourceBinding::bufferLoad(8, cs, lodObserved_.get()),
        QRhiShaderResourceBinding::bufferLoad(9, cs, priceRowSide_.get()),
        QRhiShaderResourceBinding::bufferLoad(10, cs, priceSize_.get()),
        QRhiShaderResourceBinding::bufferLoad(11, cs, priceOffsets_.get()),
        QRhiShaderResourceBinding::bufferLoad(12, cs, timePriceRowSide_.get()),
        QRhiShaderResourceBinding::bufferLoad(13, cs, timePriceSize_.get()),
        QRhiShaderResourceBinding::bufferLoad(14, cs, timePriceOffsets_.get()),
        QRhiShaderResourceBinding::bufferLoad(15, cs, dense_[0].get()),
        QRhiShaderResourceBinding::bufferLoad(16, cs, dense_[1].get()),
        QRhiShaderResourceBinding::bufferLoad(17, cs, dense_[2].get()),
        QRhiShaderResourceBinding::bufferLoad(18, cs, dense_[3].get()),
        QRhiShaderResourceBinding::bufferLoad(19, cs, dense_[4].get()),
        QRhiShaderResourceBinding::bufferLoad(20, cs, dense_[5].get()),
        QRhiShaderResourceBinding::bufferLoad(21, cs, dense_[6].get()),
        QRhiShaderResourceBinding::bufferLoad(22, cs, dense_[7].get()),
        QRhiShaderResourceBinding::bufferStore(23, cs, output_.get()),
        QRhiShaderResourceBinding::uniformBuffer(24, cs, params_.get())});
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
        QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage, params_.get())});
    if (!drawBindings_->create()) { if (error) *error = QStringLiteral("fragment bindings failed"); return false; }
    graphics_.reset(); graphicsTarget_ = nullptr;
    return true;
}

bool GpuBinner::bin(QRhiCommandBuffer *cb, const Grid &grid, QString *error) {
    if (!sourceColumns_ || !grid.columns || !grid.rows || grid.group == 0 ||
        grid.columns > 16384 || grid.rows > 16384 || grid.timeHi <= grid.timeLo) {
        if (error) *error = QStringLiteral("invalid output grid"); return false;
    }
    const uint64_t cells = uint64_t(grid.columns) * grid.rows;
    if (!output_ || output_->size() < cells * 16) {
        compute_.reset(); graphics_.reset(); computeBindings_.reset(); drawBindings_.reset(); output_.reset();
        if (!make<std::array<float, 4>>(rhi_, output_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, cells, error) ||
            !makeBindings(error)) return false;
    }
    Params params{{grid.columns, grid.rows, sourceColumns_, grid.group},
                  {grid.timeLo, grid.timeHi, grid.sizeFloor, grid.codesPerOctave},
                  {grid.rowLo, int32_t(recording::RecordingEntries::kLodMinutes),
                   int32_t(recording::RecordingEntries::kPriceBlockRows), grid.rowAlignment}};
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(params_.get(), 0, sizeof(params), &params);
    cb->beginComputePass(updates);
    cb->setComputePipeline(compute_.get());
    cb->setShaderResources(computeBindings_.get());
    cb->dispatch(int((grid.columns + 7) / 8), int((grid.rows + 7) / 8), 1);
    cb->endComputePass();
    return true;
}

bool GpuBinner::draw(QRhiCommandBuffer *cb, QRhiRenderTarget *target, QString *error) {
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
    cb->beginPass(target, QColor(7, 10, 15, 255), {1.0f, 0});
    cb->setGraphicsPipeline(graphics_.get());
    cb->setShaderResources(drawBindings_.get());
    cb->setViewport(QRhiViewport(0, 0, target->pixelSize().width(), target->pixelSize().height()));
    cb->draw(3);
    cb->endPass();
    return true;
}
} // namespace lab
