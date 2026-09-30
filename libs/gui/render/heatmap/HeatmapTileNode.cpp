#include "HeatmapTileNode.hpp"
#include "SentinelLogging.hpp"
#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace heatmap::gpu {
namespace {
struct alignas(16) DrawParams { // heatmap_display.vert/.frag uniform block (HeatmapGpuBinner DrawParams)
    float mvp[16];
    float rect[4];
    float mapping[4];
    uint32_t dims[4];
    float style[4];
};
static_assert(sizeof(DrawParams) == 128);
constexpr uint32_t kClampRows = 1; // dims.z bit 0: rows beyond the grid repeat the sentinel rows
constexpr uint32_t kBlockRows = 8192; // heatmap::tiles::kTileBlockRows (the binner allows 16384)
QShader loadShader(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}
double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace

// A resident tile: its rows split into blocks of at most kBlockRows (one buffer,
// uniform block and binding set each). The top block's rect extends up to the
// view top and the bottom block's down to the view bottom, where the clamped
// sentinel rows repeat each column's outside-the-book state.
struct HeatmapTileNode::GpuTile {
    struct Block {
        BinGrid grid; // rows [firstBin, firstBin + rows) of the tile
        std::unique_ptr<QRhiBuffer> cells, params;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        bool top = false, bottom = false;
    };
    BinGrid grid;
    std::vector<Block> blocks;
    uint64_t drawnFrame = 0;
    bool viewRows = false;
    uint64_t sourceId = 0;
    uint64_t bytes() const {
        uint64_t total = 0;
        for (const auto &b : blocks) total += (b.cells ? b.cells->size() : 0) + (b.params ? b.params->size() : 0);
        return total;
    }
};
struct HeatmapTileNode::DrawSlot {
    std::unique_ptr<QRhiBuffer> params;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
};

HeatmapTileNode::HeatmapTileNode(std::shared_ptr<HeatmapTileStats> stats)
    : stats_(stats ? std::move(stats) : std::make_shared<HeatmapTileStats>()) {}
HeatmapTileNode::~HeatmapTileNode() { releaseResources(); }

void HeatmapTileNode::releaseAll() {
    binner_.reset(); // completes its own in-flight readbacks first
    tiles_.clear();
    loadingDraws_.clear();
    loadingCell_.reset();
    pipeline_.reset();
    held_.clear();
    fading_.clear();
    tileDraws_.clear();
    drawnKey_ = 0;
    pendingGpuTile_ = 0;
    // Nothing is resident any more: the controller must not rely on GPU copies.
    stats_->complete.store(false);
    stats_->residentTiles.store(0);
    stats_->drawnKey.store(0);
    std::scoped_lock lock(stats_->mutex);
    stats_->resident.clear();
    stats_->drawn.clear();
}

void HeatmapTileNode::releaseResources() {
    if (rhi_) rhi_->removeCleanupCallback(this);
    releaseAll();
    rhi_ = nullptr;
}

void HeatmapTileNode::noteError(const QString &error) {
    stats_->errors.fetch_add(1);
    if (error != lastError_) sLog_Warning("heatmap tile node: " << error);
    lastError_ = error;
}

bool HeatmapTileNode::subRect(int64_t firstBucket, int64_t endBucket, int64_t tfMs, QRectF *rect,
                              ViewWindow *sub) const {
    const auto &view = frame_.view;
    const double span = view.timeHiMs - view.timeLoMs;
    const double a = std::max(double(firstBucket) * double(tfMs), view.timeLoMs);
    const double b = std::min(double(endBucket) * double(tfMs), view.timeHiMs);
    if (!(b > a) || !(span > 0)) return false;
    const QRectF &r = frame_.rect;
    const double x0 = r.x() + (a - view.timeLoMs) / span * r.width();
    const double x1 = r.x() + (b - view.timeLoMs) / span * r.width();
    *rect = QRectF(x0, r.y(), x1 - x0, r.height());
    *sub = {a, b, view.priceLo, view.priceHi};
    return true;
}

// Splits `tile.grid` into row blocks and bins or uploads them. viewRows: the
// grid's rows are the view plus one view height each side at the tile's tick.
bool HeatmapTileNode::binBlocks(GpuTile &tile, const TileRef &ref, QRhiCommandBuffer *cb, bool viewRows) {
    QString error;
    if (viewRows) {
        const auto &view = frame_.view;
        const double tick = ref.grid.displayTick, span = view.priceHi - view.priceLo;
        tile.grid = ref.grid;
        tile.grid.firstBin = int64_t(std::floor((view.priceLo - span) / tick)) - 1;
        const int64_t end = int64_t(std::ceil((view.priceHi + span) / tick)) + 1;
        tile.grid.rows = uint32_t(std::clamp<int64_t>(end - tile.grid.firstBin, 1, 1 << 18));
    }
    tile.blocks.clear();
    for (uint32_t top = 0; top < tile.grid.rows; top += kBlockRows) {
        GpuTile::Block block;
        block.grid = tile.grid;
        block.grid.rows = std::min(kBlockRows, tile.grid.rows - top);
        block.grid.firstBin = tile.grid.firstBin + int64_t(tile.grid.rows - top - block.grid.rows);
        block.top = top == 0;
        block.bottom = top + block.grid.rows == tile.grid.rows;
        const uint64_t bytes = uint64_t(block.grid.columns) * block.grid.rows * 4;
        block.cells.reset(rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32((bytes + 15) / 16 * 16)));
        if (!block.cells->create()) { noteError(QStringLiteral("tile buffer allocation failed")); return false; }
        tile.blocks.push_back(std::move(block));
    }
    uint32_t top = 0;
    auto *updates = ref.cells ? rhi_->nextResourceUpdateBatch() : nullptr;
    for (auto &block : tile.blocks) {
        bool ok = true;
        if (ref.cells) {
            const uint64_t bytes = uint64_t(block.grid.columns) * block.grid.rows * 4;
            updates->uploadStaticBuffer(block.cells.get(), 0, quint32(bytes), ref.cells->data() + size_t(top) * ref.grid.columns);
        } else if (viewRows) {
            ok = binner_->binResidentInto(ref.source->id, cb, block.grid, frame_.outputScale, block.cells.get(), &error);
        } else {
            ok = binner_->binInto(cb, block.grid, frame_.outputScale, block.cells.get(), &error);
        }
        if (!ok) { if (updates) updates->release(); noteError(error); return false; }
        top += block.grid.rows;
    }
    if (updates) cb->resourceUpdate(updates);
    const auto fs = QRhiShaderResourceBinding::FragmentStage, vs = QRhiShaderResourceBinding::VertexStage;
    for (auto &block : tile.blocks) {
        block.params.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(DrawParams)));
        if (!block.params->create()) { noteError(QStringLiteral("tile uniform allocation failed")); return false; }
        block.bindings.reset(rhi_->newShaderResourceBindings());
        block.bindings->setBindings({QRhiShaderResourceBinding::bufferLoad(0, fs, block.cells.get()),
                                     QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, block.params.get())});
        if (!block.bindings->create()) { noteError(QStringLiteral("tile bindings failed")); return false; }
    }
    stats_->preciseKernel.store(binner_->currentKernel() == KernelVariant::Precise);
    return true;
}

bool HeatmapTileNode::viewRowsCovered(const GpuTile &tile) const {
    const double tick = tile.grid.displayTick;
    const auto first = int64_t(std::floor(frame_.view.priceLo / tick)) - 1;
    const auto end = int64_t(std::ceil(frame_.view.priceHi / tick)) + 1;
    return first >= tile.grid.firstBin && end <= tile.grid.firstBin + int64_t(tile.grid.rows);
}

bool HeatmapTileNode::ensureResident(const TileRef &ref, QRhiCommandBuffer *cb, uint64_t &budget, bool &gpuBusy) {
    if (tiles_.count(ref.id) || !ref.id) return true;
    if (!ref.cells && !ref.source) return false;
    if (!ref.grid.columns || (!ref.viewRows && !ref.grid.rows)) return false;
    const auto started = std::chrono::steady_clock::now();
    QString error;
    auto tile = std::make_unique<GpuTile>();
    tile->grid = ref.grid;
    tile->viewRows = ref.viewRows;
    if (ref.viewRows) {
        // Hybrid: page the tile's source into the resident pool (shared by every
        // tick of this span), then bin the rows around the view.
        bool complete = false;
        const uint64_t before = budget;
        if (!binner_->uploadResident(ref.source, cb, budget, &complete, &error)) { noteError(error); return false; }
        stats_->uploadBytes.fetch_add(before - budget);
        if (!complete) return false;
        tile->sourceId = ref.source->id;
        if (!binBlocks(*tile, ref, cb, true)) return false;
        stats_->tilesBinned.fetch_add(1);
    } else if (ref.cells) {
        if (!budget) return false;
        if (ref.cells->size() * 4 < ref.cellBytes()) { noteError(QStringLiteral("tile cells smaller than grid")); return false; }
        if (!binBlocks(*tile, ref, cb, false)) return false;
        budget -= std::min(budget, ref.cellBytes());
        stats_->tilesUploaded.fetch_add(1);
        stats_->uploadBytes.fetch_add(ref.cellBytes());
    } else {
        // One GPU tile pages in at a time; the binner's active source may already
        // be this tile's source (a tick change re-bins it without an upload).
        if (gpuBusy && pendingGpuTile_ != ref.id) return false;
        if (binner_->activeSource() != ref.source) {
            if (!budget) return false;
            if (!binner_->setSource(ref.source, &error)) { noteError(error); return false; }
            const uint64_t before = stats_->uploadBytes.load();
            if (!binner_->uploadStep(cb, budget, &error)) { noteError(error); return false; }
            stats_->uploadBytes.store(before + std::min<uint64_t>(budget, ref.source->bytes()));
            budget = 0; // the step used the remaining budget
            if (binner_->uploadPending()) {
                pendingGpuTile_ = ref.id;
                gpuBusy = true;
                return false;
            }
        }
        pendingGpuTile_ = 0;
        if (!binBlocks(*tile, ref, cb, false)) return false;
        stats_->tilesBinned.fetch_add(1);
    }
    tiles_[ref.id] = std::move(tile);
    stats_->lastTileMs.store(msSince(started));
    return true;
}

bool HeatmapTileNode::ensurePipeline(QRhiRenderPassDescriptor *pass, int samples) {
    const auto format = pass->serializedFormat();
    if (pipeline_ && pipelineFormat_ == format && pipelineSamples_ == samples) return true;
    const QShader vs = loadShader(QStringLiteral(":/heatmapgpu/heatmap_display.vert.qsb"));
    const QShader fs = loadShader(QStringLiteral(":/heatmapgpu/heatmap_display.frag.qsb"));
    if (!vs.isValid() || !fs.isValid() || loadingDraws_.empty()) return false;
    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setShaderResourceBindings(loadingDraws_.front()->bindings.get()); // layout of every draw
    pipeline_->setRenderPassDescriptor(pass);
    pipeline_->setSampleCount(samples);
    pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
    QRhiGraphicsPipeline::TargetBlend blend; // premultiplied alpha, as the scene graph
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    pipeline_->setTargetBlends({blend});
    if (!pipeline_->create()) {
        pipeline_.reset();
        return false;
    }
    pipelineFormat_ = format;
    pipelineSamples_ = samples;
    return true;
}

void HeatmapTileNode::addTileDraw(QRhiResourceUpdateBatch *updates, GpuTile &tile, int64_t firstBucket,
                                  int64_t endBucket, int64_t tfMs, float opacity) {
    const uint64_t frame = stats_->frames.load();
    if (tile.drawnFrame == frame) return; // one uniform block per tile and frame
    QRectF r;
    ViewWindow sub;
    if (!subRect(firstBucket, endBucket, tfMs, &r, &sub)) return;
    const auto &view = frame_.view;
    const double priceSpan = view.priceHi - view.priceLo;
    if (!(priceSpan > 0)) return;
    const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
    bool any = false;
    for (auto &block : tile.blocks) {
        // Price rows of this block; the outer blocks extend to the view edges.
        const double lo = block.bottom ? view.priceLo : std::max(view.priceLo, double(block.grid.firstBin) * block.grid.displayTick);
        const double hi = block.top ? view.priceHi
                                    : std::min(view.priceHi, double(block.grid.firstBin + int64_t(block.grid.rows)) * block.grid.displayTick);
        if (!(hi > lo)) continue;
        const double y0 = r.y() + (view.priceHi - hi) / priceSpan * r.height();
        const double y1 = r.y() + (view.priceHi - lo) / priceSpan * r.height();
        ViewWindow blockView = sub;
        blockView.priceLo = lo;
        blockView.priceHi = hi;
        const auto mapping = mappingFor(block.grid, blockView);
        DrawParams p{};
        std::memcpy(p.mvp, mvp.constData(), sizeof(p.mvp));
        p.rect[0] = float(r.x()); p.rect[1] = float(y0); p.rect[2] = float(r.width()); p.rect[3] = float(y1 - y0);
        p.mapping[0] = mapping.timeOffset; p.mapping[1] = mapping.timeSpan;
        p.mapping[2] = mapping.priceOffset; p.mapping[3] = mapping.priceSpan;
        p.dims[0] = block.grid.columns; p.dims[1] = block.grid.rows; p.dims[2] = kClampRows;
        p.style[0] = frame_.style.codeFloor; p.style[1] = frame_.style.codeRange; p.style[2] = opacity * frame_.style.opacity;
        updates->updateDynamicBuffer(block.params.get(), 0, sizeof(p), &p);
        tileDraws_.push_back({block.bindings.get(), opacity});
        any = true;
    }
    if (any) tile.drawnFrame = frame;
}

bool HeatmapTileNode::addLoadingSlot() {
    auto slot = std::make_unique<DrawSlot>();
    slot->params.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(DrawParams)));
    if (!slot->params->create()) { noteError(QStringLiteral("loading uniform allocation failed")); return false; }
    slot->bindings.reset(rhi_->newShaderResourceBindings());
    const auto fs = QRhiShaderResourceBinding::FragmentStage, vs = QRhiShaderResourceBinding::VertexStage;
    slot->bindings->setBindings({QRhiShaderResourceBinding::bufferLoad(0, fs, loadingCell_.get()),
                                 QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, slot->params.get())});
    if (!slot->bindings->create()) { noteError(QStringLiteral("loading bindings failed")); return false; }
    loadingDraws_.push_back(std::move(slot));
    return true;
}

void HeatmapTileNode::addLoadingDraw(QRhiResourceUpdateBatch *updates, int64_t firstBucket, int64_t endBucket,
                                     int64_t tfMs) {
    QRectF r;
    ViewWindow sub;
    if (!subRect(firstBucket, endBucket, tfMs, &r, &sub)) return;
    if (loadingUsed_ == loadingDraws_.size() && !addLoadingSlot()) return;
    DrawParams p{};
    const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
    std::memcpy(p.mvp, mvp.constData(), sizeof(p.mvp));
    p.rect[0] = float(r.x()); p.rect[1] = float(r.y()); p.rect[2] = float(r.width()); p.rect[3] = float(r.height());
    p.mapping[0] = 0; p.mapping[1] = 0.999f; p.mapping[2] = 0; p.mapping[3] = 1;
    p.dims[0] = 1; p.dims[1] = 1; p.dims[2] = kClampRows;
    p.style[0] = frame_.style.codeFloor; p.style[1] = frame_.style.codeRange; p.style[2] = frame_.style.opacity;
    updates->updateDynamicBuffer(loadingDraws_[loadingUsed_]->params.get(), 0, sizeof(p), &p);
    ++loadingUsed_;
}

void HeatmapTileNode::prepare() {
    const auto started = std::chrono::steady_clock::now();
    tileDraws_.clear();
    loadingUsed_ = 0;
    QRhiCommandBuffer *cb = commandBuffer();
    QRhiRenderTarget *rt = renderTarget();
    if (!cb || !rt) return;
    QRhi *rhi = rt->rhi();
    if (rhi != rhi_) {
        if (rhi_) rhi_->removeCleanupCallback(this);
        releaseAll();
        rhi_ = rhi;
        // Scene graph invalidation can destroy the QRhi before this node: free
        // every resource while it still works.
        rhi_->addCleanupCallback(this, [this](QRhi *) { releaseAll(); rhi_ = nullptr; });
    }
    stats_->frames.fetch_add(1);
    if (!binner_) binner_ = std::make_unique<HeatmapGpuBinner>(rhi_);
    binner_->setMemoryCap(frame_.binnerMemoryCapBytes);
    binner_->runPrecisionSelfTest(cb);
    if (!loadingCell_) {
        loadingCell_.reset(rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, 16));
        if (!loadingCell_->create()) return noteError(QStringLiteral("loading cell allocation failed"));
        const uint32_t cell = uint32_t(CellState::Loading) << 16;
        auto *updates = rhi_->nextResourceUpdateBatch();
        updates->uploadStaticBuffer(loadingCell_.get(), 0, 4, &cell);
        cb->resourceUpdate(updates);
        // The graphics pipeline takes its binding layout from the first loading
        // slot, so it exists before any draw needs it.
        if (!addLoadingSlot()) return;
    }

    // 1. Free what the controller no longer keeps.
    std::unordered_map<uint64_t, const TileRef *> refs;
    for (const auto &ref : frame_.tiles) refs[ref.id] = &ref;
    for (auto it = tiles_.begin(); it != tiles_.end();)
        it = refs.count(it->first) ? std::next(it) : tiles_.erase(it);
    if (pendingGpuTile_ && !refs.count(pendingGpuTile_)) pendingGpuTile_ = 0;

    // 2. Make tiles resident: visible primaries, then fallbacks, then prefetch.
    uint64_t budget = std::max<uint64_t>(frame_.uploadBudgetBytes, 1);
    bool gpuBusy = pendingGpuTile_ != 0;
    std::vector<const TileRef *> order;
    std::unordered_set<uint64_t> queued;
    auto queue = [&](uint64_t id) {
        const auto it = refs.find(id);
        if (it != refs.end() && !tiles_.count(id) && queued.insert(id).second) order.push_back(it->second);
    };
    if (pendingGpuTile_) queue(pendingGpuTile_); // finish the upload in flight first
    for (const auto &slot : frame_.visible) queue(slot.primary);
    for (const auto &slot : frame_.visible) queue(slot.fallback);
    for (const auto &ref : frame_.tiles) queue(ref.id);
    for (const auto *ref : order) {
        const bool free = ref->source && (binner_->activeSource() == ref->source ||
                                          (ref->viewRows && binner_->isResident(ref->source->id)));
        if (!budget && !free) continue;
        ensureResident(*ref, cb, budget, gpuBusy);
    }

    // Hybrid: tiles whose binned rows no longer cover the view re-bin in place
    // (their source is resident: one compute pass, no upload).
    {
        std::vector<uint64_t> sources;
        for (const auto &ref : frame_.tiles) if (ref.viewRows && ref.source) sources.push_back(ref.source->id);
        binner_->releaseResidentExcept(sources);
        auto rebin = [&](uint64_t id) {
            const auto it = tiles_.find(id);
            const auto ref = refs.find(id);
            if (it == tiles_.end() || ref == refs.end() || !it->second->viewRows || viewRowsCovered(*it->second)) return;
            if (!binner_->isResident(it->second->sourceId)) return;
            if (binBlocks(*it->second, *ref->second, cb, true)) stats_->rebinnedTiles.fetch_add(1);
        };
        for (const auto &slot : frame_.visible) { rebin(slot.primary); rebin(slot.fallback); }
        for (const auto &d : held_) rebin(d.id);
        for (const auto &d : fading_) rebin(d.id);
    }

    // 3. Transitions.
    auto resident = [&](uint64_t id) { return id && tiles_.count(id); };
    bool allReady = true;
    for (const auto &slot : frame_.visible)
        if (slot.expected && !resident(slot.primary)) allReady = false;
    bool holding = false;
    if (frame_.key != drawnKey_) {
        if (allReady) {
            const bool fade = drawnKey_ && drawnTfMs_ == frame_.tfMs && frame_.crossfadeMs > 0 && !held_.empty();
            fading_ = fade ? held_ : std::vector<Draw>{};
            fadeStart_ = std::chrono::steady_clock::now();
            drawnKey_ = frame_.key;
            drawnTfMs_ = frame_.tfMs;
        } else {
            holding = !held_.empty();
        }
    }
    float fadeOpacity = 0;
    if (!fading_.empty()) {
        const double t = msSince(fadeStart_) / std::max(frame_.crossfadeMs, 1e-3);
        if (t >= 1) fading_.clear();
        else fadeOpacity = float(1 - t);
    }

    // 4. Draw list. Pan and zoom only change these mappings.
    auto *updates = rhi_->nextResourceUpdateBatch();
    uint32_t primaryCount = 0, fallbackCount = 0, loadingCount = 0;
    std::vector<uint64_t> drawn;
    if (holding) {
        for (const auto &d : held_) {
            if (auto it = tiles_.find(d.id); it != tiles_.end()) {
                addTileDraw(updates, *it->second, d.firstBucket, d.endBucket, d.tfMs, 1.0f);
                drawn.push_back(d.id);
            }
        }
        // New time the held picture does not cover draws the loading hatch.
        for (const auto &slot : frame_.visible) {
            if (!slot.expected) continue;
            const double a = double(slot.firstBucket) * double(frame_.tfMs), b = double(slot.endBucket) * double(frame_.tfMs);
            bool covered = false;
            for (const auto &d : held_)
                covered |= double(d.firstBucket) * double(d.tfMs) <= a && double(d.endBucket) * double(d.tfMs) >= b;
            if (!covered) { addLoadingDraw(updates, slot.firstBucket, slot.endBucket, frame_.tfMs); ++loadingCount; }
        }
    } else {
        std::vector<Draw> now;
        for (const auto &slot : frame_.visible) {
            uint64_t id = 0;
            if (resident(slot.primary)) { id = slot.primary; ++primaryCount; }
            else if (resident(slot.fallback)) { id = slot.fallback; ++fallbackCount; }
            if (id) {
                addTileDraw(updates, *tiles_[id], slot.firstBucket, slot.endBucket, frame_.tfMs, 1.0f);
                now.push_back({id, slot.firstBucket, slot.endBucket, frame_.tfMs});
                drawn.push_back(id);
            } else if (slot.expected) {
                addLoadingDraw(updates, slot.firstBucket, slot.endBucket, frame_.tfMs);
                ++loadingCount;
            }
        }
        held_ = std::move(now); // the newest picture of this target
    }
    for (const auto &d : fading_) {
        if (auto it = tiles_.find(d.id); it != tiles_.end()) {
            addTileDraw(updates, *it->second, d.firstBucket, d.endBucket, d.tfMs, fadeOpacity);
            drawn.push_back(d.id);
        }
    }
    cb->resourceUpdate(updates);
    if (!ensurePipeline(rt->renderPassDescriptor(), rt->sampleCount()) && !loadingDraws_.empty())
        noteError(QStringLiteral("heatmap tile pipeline failed"));

    uint64_t tileBytes = 0;
    std::vector<uint64_t> residentIds;
    residentIds.reserve(tiles_.size());
    for (const auto &[id, tile] : tiles_) { tileBytes += tile->bytes(); residentIds.push_back(id); }
    for (const auto &slot : loadingDraws_) tileBytes += slot->params ? slot->params->size() : 0;
    {
        std::scoped_lock lock(stats_->mutex);
        stats_->drawn = std::move(drawn);
        stats_->resident = std::move(residentIds);
    }
    stats_->residentTiles.store(tiles_.size());
    stats_->residentBytes.store(tileBytes);
    stats_->binnerBytes.store(binner_->gpuBytes());
    stats_->gpuBytes.store(tileBytes + binner_->gpuBytes() + (loadingCell_ ? loadingCell_->size() : 0));
    stats_->slotCount.store(uint32_t(frame_.visible.size()));
    stats_->drawnPrimary.store(primaryCount);
    stats_->drawnFallback.store(fallbackCount);
    stats_->loadingSlots.store(loadingCount);
    stats_->holding.store(holding);
    stats_->crossfading.store(!fading_.empty());
    stats_->drawnKey.store(drawnKey_);
    stats_->complete.store(!holding && drawnKey_ == frame_.key && !loadingCount && !fallbackCount);
    stats_->gpuFrameMs.store(cb->lastCompletedGpuTime() * 1000.0);
    stats_->prepareMs.store(msSince(started));
}

void HeatmapTileNode::render(const RenderState *state) {
    if (!pipeline_ || (tileDraws_.empty() && !loadingUsed_)) return;
    QRhiCommandBuffer *cb = commandBuffer();
    const QSize size = renderTarget()->pixelSize();
    cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
    if (state && state->scissorEnabled()) {
        const QRect r = state->scissorRect();
        cb->setScissor(QRhiScissor(r.x(), r.y(), r.width(), r.height()));
    } else {
        cb->setScissor(QRhiScissor(0, 0, size.width(), size.height()));
    }
    cb->setGraphicsPipeline(pipeline_.get());
    // Opaque tiles first, then loading hatches, then the fading-out set (tileDraws_
    // lists it last) blended over them.
    size_t firstFading = tileDraws_.size();
    for (size_t i = 0; i < tileDraws_.size(); ++i)
        if (tileDraws_[i].second < 1.0f) { firstFading = i; break; }
    for (size_t i = 0; i < firstFading; ++i) {
        cb->setShaderResources(tileDraws_[i].first);
        cb->draw(4);
    }
    for (size_t i = 0; i < loadingUsed_; ++i) {
        cb->setShaderResources(loadingDraws_[i]->bindings.get());
        cb->draw(4);
    }
    for (size_t i = firstFading; i < tileDraws_.size(); ++i) {
        cb->setShaderResources(tileDraws_[i].first);
        cb->draw(4);
    }
}
} // namespace heatmap::gpu
