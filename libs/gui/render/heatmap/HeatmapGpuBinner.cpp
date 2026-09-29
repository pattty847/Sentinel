#include "HeatmapGpuBinner.hpp"
#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>

namespace heatmap::gpu {
namespace {
QShader loadShader(const char *path) {
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
std::unique_ptr<QRhiBuffer> makeBuffer(QRhi *rhi, QRhiBuffer::Type type, QRhiBuffer::UsageFlags usage,
                                       uint64_t bytes, QString *error) {
    bytes = std::max<uint64_t>((bytes + 15) / 16 * 16, 16);
    if (bytes > std::numeric_limits<quint32>::max()) {
        fail(error, QStringLiteral("GPU buffer exceeds 4 GiB"));
        return {};
    }
    std::unique_ptr<QRhiBuffer> buffer(rhi->newBuffer(type, usage, quint32(bytes)));
    if (!buffer || !buffer->create()) {
        fail(error, QStringLiteral("GPU buffer allocation failed (%1 bytes)").arg(bytes));
        return {};
    }
    return buffer;
}
struct alignas(16) ComputeParams {
    std::array<uint32_t, 4> dims;
    std::array<int32_t, 4> time;
    std::array<int32_t, 4> price;
    std::array<uint32_t, kMaxTicks> factors;
};
struct alignas(16) DrawParams {
    float mvp[16];
    float rect[4];
    float mapping[4];
    uint32_t dims[4];
    float style[4];
};
static_assert(sizeof(ComputeParams) == 112 && sizeof(DrawParams) == 128);

int32_t clampToInt32(double value, double lo, double hi) {
    if (std::isnan(value)) return 0;
    return int32_t(std::clamp(value, lo, hi));
}
// The threshold table is ~256 KiB of pure CPU work per size scale; share it.
const std::vector<FloatFloat> &cachedThresholds(const recording::SizeScale &scale) {
    static std::mutex mutex;
    static std::map<std::pair<double, double>, std::vector<FloatFloat>> cache;
    std::scoped_lock lock(mutex);
    auto &entry = cache[{scale.floor, scale.codesPerOctave}];
    if (entry.empty()) entry = encodeThresholds(scale);
    return entry;
}
} // namespace

struct HeatmapGpuBinner::SourceBuffers {
    std::shared_ptr<const GpuSource> source;
    std::unique_ptr<QRhiBuffer> bucketSlots, columnGroups, groups, runs, entries;
    struct Part { QRhiBuffer *buffer; const char *data; uint64_t bytes; };
    std::vector<Part> parts;
    size_t part = 0;
    uint64_t offset = 0;
    bool complete() const { return part >= parts.size(); }
    uint64_t bytes() const {
        uint64_t total = 0;
        for (auto *b : {bucketSlots.get(), columnGroups.get(), groups.get(), runs.get(), entries.get()})
            if (b) total += b->size();
        return total;
    }
};

HeatmapGpuBinner::HeatmapGpuBinner(QRhi *rhi) : rhi_(rhi) {}
HeatmapGpuBinner::~HeatmapGpuBinner() = default;

std::shared_ptr<const GpuSource> HeatmapGpuBinner::activeSource() const {
    return active_ ? active_->source : nullptr;
}
std::shared_ptr<const GpuSource> HeatmapGpuBinner::pendingSource() const {
    return pending_ ? pending_->source : nullptr;
}
uint64_t HeatmapGpuBinner::sourceBytes() const { return active_ ? active_->bytes() : 0; }
uint64_t HeatmapGpuBinner::gpuBytes() const {
    uint64_t total = (active_ ? active_->bytes() : 0) + (pending_ ? pending_->bytes() : 0);
    for (auto *b : {thresholds_.get(), output_.get(), computeParams_.get(), drawParams_.get()})
        if (b) total += b->size();
    return total;
}

bool HeatmapGpuBinner::setSource(std::shared_ptr<const GpuSource> source, QString *error) {
    if (!source) return true;
    if ((active_ && active_->source == source) || (pending_ && pending_->source == source)) return true;
    if (source->columnGroups.empty() || source->bucketSlots.empty())
        return fail(error, QStringLiteral("empty heatmap GPU source"));
    auto buffers = std::make_unique<SourceBuffers>();
    buffers->source = source;
    const auto storage = QRhiBuffer::StorageBuffer;
    auto make = [&](std::unique_ptr<QRhiBuffer> &dst, const void *data, uint64_t bytes) {
        dst = makeBuffer(rhi_, QRhiBuffer::Static, storage, bytes, error);
        if (dst && bytes) buffers->parts.push_back({dst.get(), static_cast<const char *>(data), bytes});
        return dst != nullptr;
    };
    const auto &s = *source;
    if (!make(buffers->bucketSlots, s.bucketSlots.data(), s.bucketSlots.size() * 4ull) ||
        !make(buffers->columnGroups, s.columnGroups.data(), s.columnGroups.size() * 4ull) ||
        !make(buffers->groups, s.groups.data(), s.groups.size() * sizeof(GroupMeta)) ||
        !make(buffers->runs, s.runs.data(), s.runs.size() * 8ull) ||
        !make(buffers->entries, s.entries.data(), s.entries.size() * 4ull)) return false;
    pending_ = std::move(buffers); // a superseded pending upload is simply dropped
    return true;
}

bool HeatmapGpuBinner::uploadStep(QRhiCommandBuffer *cb, uint64_t budgetBytes, QString *error) {
    if (!pending_) return true;
    if (!cb || !budgetBytes) return fail(error, QStringLiteral("invalid upload step"));
    auto &p = *pending_;
    auto *updates = rhi_->nextResourceUpdateBatch();
    uint64_t remaining = budgetBytes;
    while (!p.complete() && remaining) {
        const auto &part = p.parts[p.part];
        const uint64_t chunk = std::min(part.bytes - p.offset, remaining);
        updates->uploadStaticBuffer(part.buffer, quint32(p.offset), quint32(chunk), part.data + p.offset);
        p.offset += chunk;
        remaining -= chunk;
        if (p.offset == part.bytes) { ++p.part; p.offset = 0; }
    }
    cb->resourceUpdate(updates);
    if (p.complete()) {
        active_ = std::move(pending_);
        computeBoundTo_ = nullptr; // bindings reference the old source buffers
    }
    return true;
}

bool HeatmapGpuBinner::uploadAll(QRhiCommandBuffer *cb, QString *error) {
    while (pending_)
        if (!uploadStep(cb, 64ull << 20, error)) return false;
    return true;
}

bool HeatmapGpuBinner::rebuildComputeBindings(QString *error) {
    const auto cs = QRhiShaderResourceBinding::ComputeStage;
    auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::bufferLoad(0, cs, active_->bucketSlots.get()),
        QRhiShaderResourceBinding::bufferLoad(1, cs, active_->columnGroups.get()),
        QRhiShaderResourceBinding::bufferLoad(2, cs, active_->groups.get()),
        QRhiShaderResourceBinding::bufferLoad(3, cs, active_->runs.get()),
        QRhiShaderResourceBinding::bufferLoad(4, cs, active_->entries.get()),
        QRhiShaderResourceBinding::bufferLoad(5, cs, thresholds_.get()),
        QRhiShaderResourceBinding::bufferStore(6, cs, output_.get()),
        QRhiShaderResourceBinding::uniformBuffer(7, cs, computeParams_.get())});
    if (!bindings->create()) return fail(error, QStringLiteral("heatmap compute bindings failed"));
    if (!compute_) {
        const QShader shader = loadShader(":/heatmapgpu/heatmap_bin.comp.qsb");
        if (!shader.isValid()) return fail(error, QStringLiteral("heatmap_bin.comp.qsb missing"));
        compute_.reset(rhi_->newComputePipeline());
        compute_->setShaderStage({QRhiShaderStage::Compute, shader});
        compute_->setShaderResourceBindings(bindings.get());
        if (!compute_->create()) return fail(error, QStringLiteral("heatmap compute pipeline failed"));
    }
    computeBindings_ = std::move(bindings); // layout-compatible with the pipeline's
    computeBoundTo_ = active_.get();
    return true;
}

bool HeatmapGpuBinner::rebuildDrawBindings(QString *error) {
    const auto fs = QRhiShaderResourceBinding::FragmentStage;
    const auto vs = QRhiShaderResourceBinding::VertexStage;
    auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
    bindings->setBindings({QRhiShaderResourceBinding::bufferLoad(0, fs, output_.get()),
                           QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, drawParams_.get())});
    if (!bindings->create()) return fail(error, QStringLiteral("heatmap draw bindings failed"));
    drawBindings_ = std::move(bindings);
    return true;
}

bool HeatmapGpuBinner::bin(QRhiCommandBuffer *cb, const BinGrid &grid,
                           const recording::SizeScale &outputScale, QString *error) {
    if (!active_) return fail(error, QStringLiteral("no uploaded heatmap source"));
    const auto &source = *active_->source;
    if (!cb || grid.tfMs != source.tfMs || !grid.columns || !grid.rows ||
        grid.columns > kMaxGridColumns || grid.rows > kMaxGridRows || !(grid.displayTick > 0))
        return fail(error, QStringLiteral("invalid heatmap output grid"));
    if (!computeParams_) {
        computeParams_ = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ComputeParams), error);
        drawParams_ = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(DrawParams), error);
        if (!computeParams_ || !drawParams_) return false;
    }
    auto *updates = rhi_->nextResourceUpdateBatch();
    if (!thresholds_ || thresholdScale_.floor != outputScale.floor ||
        thresholdScale_.codesPerOctave != outputScale.codesPerOctave) {
        const auto &table = cachedThresholds(outputScale);
        if (!thresholds_) {
            thresholds_ = makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer,
                                     table.size() * sizeof(FloatFloat), error);
            if (!thresholds_) { updates->release(); return false; }
            computeBoundTo_ = nullptr;
        }
        updates->uploadStaticBuffer(thresholds_.get(), 0, quint32(table.size() * sizeof(FloatFloat)), table.data());
        thresholdScale_ = outputScale;
    }
    const uint64_t cellBytes = uint64_t(grid.columns) * grid.rows * 4;
    if (!output_ || output_->size() < cellBytes) {
        // Grow only (with headroom); source buffers are untouched.
        output_ = makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, cellBytes + cellBytes / 4, error);
        if (!output_) { updates->release(); return false; }
        computeBoundTo_ = nullptr;
        drawBindings_.reset();
    }
    if (computeBoundTo_ != active_.get() && !rebuildComputeBindings(error)) { updates->release(); return false; }
    if (!drawBindings_ && !rebuildDrawBindings(error)) { updates->release(); return false; }

    const auto factors = tickFactors(source, grid.displayTick);
    uint32_t maxFactor = 0;
    for (const auto f : factors) maxFactor = std::max(maxFactor, f);
    const double topRow = double(std::max(std::abs(grid.firstBin), std::abs(grid.firstBin + grid.rows))) * maxFactor;
    if (topRow >= double(std::numeric_limits<int32_t>::max())) {
        updates->release();
        return fail(error, QStringLiteral("heatmap grid native rows exceed int32"));
    }
    constexpr double big = double(1 << 30);
    ComputeParams params{};
    params.dims = {grid.columns, grid.rows, uint32_t(source.bucketSlots.size()), source.wide ? 1u : 0u};
    params.time = {clampToInt32(double(grid.firstBucket - source.firstBucket), -big, big),
                   clampToInt32(double(source.availableFirstBucket - grid.firstBucket), 0, grid.columns),
                   clampToInt32(double(source.availableEndBucket - grid.firstBucket), 0, grid.columns),
                   int32_t(thresholds_->size() / sizeof(FloatFloat) >= recording::kMaxCode - 1 ?
                           recording::kMaxCode - 1 : 0)};
    params.price = {int32_t(grid.firstBin),
                    clampToInt32(std::ceil(source.clipPriceLo / grid.displayTick - 1e-9), -big, big),
                    clampToInt32(std::floor(source.clipPriceHi / grid.displayTick + 1e-9), -big, big), 0};
    params.factors = factors;
    updates->updateDynamicBuffer(computeParams_.get(), 0, sizeof(params), &params);
    cb->beginComputePass(updates);
    cb->setComputePipeline(compute_.get());
    cb->setShaderResources(computeBindings_.get());
    cb->dispatch(int((grid.columns + 7) / 8), int((grid.rows + 7) / 8), 1);
    cb->endComputePass();
    binnedGrid_ = grid;
    binnedSourceId_ = source.id;
    return true;
}

bool HeatmapGpuBinner::readBack(QRhiCommandBuffer *cb, QRhiReadbackResult *result, QString *error) {
    if (!binnedGrid_ || !output_ || !result || !cb) return fail(error, QStringLiteral("nothing binned to read back"));
    const uint64_t bytes = uint64_t(binnedGrid_->columns) * binnedGrid_->rows * 4;
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->readBackBuffer(output_.get(), 0, quint32(bytes), result);
    cb->resourceUpdate(updates);
    return true;
}

bool HeatmapGpuBinner::prepareDraw(QRhiRenderPassDescriptor *pass, int sampleCount, QString *error) {
    if (!drawBindings_) return fail(error, QStringLiteral("bin before drawing"));
    const auto format = pass->serializedFormat();
    if (graphics_ && graphicsFormat_ == format && graphicsSamples_ == sampleCount) return true;
    const QShader vs = loadShader(":/heatmapgpu/heatmap_display.vert.qsb");
    const QShader fs = loadShader(":/heatmapgpu/heatmap_display.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) return fail(error, QStringLiteral("heatmap display shaders missing"));
    graphics_.reset(rhi_->newGraphicsPipeline());
    graphics_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    graphics_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    graphics_->setShaderResourceBindings(drawBindings_.get());
    graphics_->setRenderPassDescriptor(pass);
    graphics_->setSampleCount(sampleCount);
    graphics_->setFlags(QRhiGraphicsPipeline::UsesScissor);
    QRhiGraphicsPipeline::TargetBlend blend; // premultiplied alpha, as the scene graph
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    graphics_->setTargetBlends({blend});
    if (!graphics_->create()) {
        graphics_.reset();
        return fail(error, QStringLiteral("heatmap display pipeline failed"));
    }
    graphicsFormat_ = format;
    graphicsSamples_ = sampleCount;
    return true;
}

void HeatmapGpuBinner::updateDraw(QRhiResourceUpdateBatch *updates, const QMatrix4x4 &mvp, const QRectF &itemRect,
                                  const DisplayMapping &mapping, const DrawStyle &style) {
    if (!drawParams_ || !binnedGrid_) return;
    DrawParams params{};
    std::memcpy(params.mvp, mvp.constData(), sizeof(params.mvp));
    params.rect[0] = float(itemRect.x()); params.rect[1] = float(itemRect.y());
    params.rect[2] = float(itemRect.width()); params.rect[3] = float(itemRect.height());
    params.mapping[0] = mapping.timeOffset; params.mapping[1] = mapping.timeSpan;
    params.mapping[2] = mapping.priceOffset; params.mapping[3] = mapping.priceSpan;
    params.dims[0] = binnedGrid_->columns; params.dims[1] = binnedGrid_->rows;
    params.style[0] = style.codeFloor; params.style[1] = style.codeRange;
    updates->updateDynamicBuffer(drawParams_.get(), 0, sizeof(params), &params);
}

void HeatmapGpuBinner::recordDraw(QRhiCommandBuffer *cb) {
    if (!canDraw()) return;
    cb->setGraphicsPipeline(graphics_.get());
    cb->setShaderResources(drawBindings_.get());
    cb->draw(4);
}
} // namespace heatmap::gpu
