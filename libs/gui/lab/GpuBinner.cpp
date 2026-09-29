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
    col_.reset(); row_.reset(); side_.reset(); size_.reset(); offsets_.reset(); coverage_.reset(); observed_.reset(); output_.reset(); params_.reset();
    sourceColumns_ = data.columns(); bytes_ = 0;
    if (!sourceColumns_ || data.column.size() != data.row.size() ||
        data.column.size() != data.side.size() || data.column.size() != data.size.size() ||
        data.offsets.size() != size_t(sourceColumns_) + 1 || data.coverage.size() != sourceColumns_ ||
        data.observedMs.size() != sourceColumns_) {
        if (error) *error = QStringLiteral("invalid sparse entry arrays");
        return false;
    }
    const auto storage = QRhiBuffer::StorageBuffer;
    if (!make<uint32_t>(rhi_, col_, QRhiBuffer::Static, storage, data.column.size(), error) ||
        !make<uint32_t>(rhi_, row_, QRhiBuffer::Static, storage, data.row.size(), error) ||
        !make<uint32_t>(rhi_, side_, QRhiBuffer::Static, storage, data.side.size(), error) ||
        !make<float>(rhi_, size_, QRhiBuffer::Static, storage, data.size.size(), error) ||
        !make<uint32_t>(rhi_, offsets_, QRhiBuffer::Static, storage, data.offsets.size(), error) ||
        !make<recording::RecordingEntries::Coverage>(rhi_, coverage_, QRhiBuffer::Static, storage, data.coverage.size(), error) ||
        !make<uint32_t>(rhi_, observed_, QRhiBuffer::Static, storage, data.observedMs.size(), error) ||
        !make<Params>(rhi_, params_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 1, error)) return false;
    auto *updates = rhi_->nextResourceUpdateBatch();
    if (!data.column.empty()) {
        updates->uploadStaticBuffer(col_.get(), data.column.data());
        updates->uploadStaticBuffer(row_.get(), data.row.data());
        updates->uploadStaticBuffer(side_.get(), data.side.data());
        updates->uploadStaticBuffer(size_.get(), data.size.data());
    }
    updates->uploadStaticBuffer(offsets_.get(), data.offsets.data());
    updates->uploadStaticBuffer(coverage_.get(), data.coverage.data());
    updates->uploadStaticBuffer(observed_.get(), data.observedMs.data());
    cb->resourceUpdate(updates);
    bytes_ = data.gpuBytes();
    return true;
}

bool GpuBinner::makeBindings(QString *error) {
    computeBindings_.reset(rhi_->newShaderResourceBindings());
    const auto cs = QRhiShaderResourceBinding::ComputeStage;
    computeBindings_->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, cs, col_.get()),
        QRhiShaderResourceBinding::bufferLoad(1, cs, row_.get()),
        QRhiShaderResourceBinding::bufferLoad(2, cs, side_.get()),
        QRhiShaderResourceBinding::bufferLoad(3, cs, size_.get()),
        QRhiShaderResourceBinding::bufferLoad(4, cs, offsets_.get()),
        QRhiShaderResourceBinding::bufferLoad(5, cs, coverage_.get()),
        QRhiShaderResourceBinding::bufferLoad(6, cs, observed_.get()),
        QRhiShaderResourceBinding::bufferStore(7, cs, output_.get()),
        QRhiShaderResourceBinding::uniformBuffer(8, cs, params_.get())});
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
                  {grid.rowLo, 0, 0, 0}};
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
