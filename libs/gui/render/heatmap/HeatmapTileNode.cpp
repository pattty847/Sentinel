#include "HeatmapTileNode.hpp"
#include "SentinelLogging.hpp"
#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace heatmap::gpu {
namespace {
struct alignas(16) DrawParams { // heatmap_display.vert/.frag uniform block
    float mvp[16];
    float rect[4];
    float mapping[4];
    uint32_t dims[4];
    float style[4];
};
static_assert(sizeof(DrawParams) == 128);
constexpr uint32_t kClampRows = 1;    // dims.z bit 0: rows beyond the grid repeat the sentinel rows
constexpr uint32_t kBlockRows = 8192; // heatmap::tiles::kTileBlockRows (the binner allows 16384)
constexpr uint32_t kMaxRows = 1u << 18;
constexpr size_t kMaxSpareBuffers = 64;
constexpr uint64_t kSourceOverheadBytes = 256; // per-buffer rounding of a resident source (13 buffers x 16)
std::atomic<bool> retentionDisabled{false};
QShader loadShader(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}
double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
bool guarded(SpanTier tier) { return tier == SpanTier::Visible || tier == SpanTier::Fallback; }
// Eviction preference: higher goes first (unlisted, recent-tf, prefetch far to near).
std::pair<int, int64_t> evictionOrder(bool listed, const SpanRank &rank) {
    if (!listed) return {3, 0};
    if (rank.tier == SpanTier::RecentTf) return {2, rank.distance};
    if (rank.tier == SpanTier::Prefetch) return {1, rank.distance};
    return {0, 0};
}
} // namespace

void HeatmapTileNode::setRetentionDisabledForTest(bool disabled) { retentionDisabled.store(disabled); }

// A span source as the node knows it: listed by the snapshot, and/or resident.
struct HeatmapTileNode::Source {
    SpanSourceKey key;
    SpanSourceBuildPtr build;    // the latest listed build (gpu == nullptr once released)
    uint64_t sourceId = 0;       // binner resident id (the uploaded image's GpuSource::id)
    uint64_t bytes = 0;          // resident buffer bytes once created
    bool created = false, complete = false;
    bool reportedUploaded = false; // the controller was (or will be) told it is uploaded
    int64_t bandLo = 0, bandEnd = 0; // union of full-coverage bands over the span (price units)
    uint64_t listedVersion = 0;  // snapshot version that last listed it
    SpanRank rank;
    uint64_t pinnedFrame = 0;    // a drawn bin used it this frame
    uint64_t uploadFrame = 0;    // last frame that recorded an upload into it
};

// Cells of one span at one tick: the rows around the view, binned from `passes`
// (first source, then fill passes), split into row blocks.
struct HeatmapTileNode::Bin {
    struct Block {
        BinGrid grid;
        std::unique_ptr<QRhiBuffer> cells, params;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        QRhiBuffer *boundTo = nullptr;
        bool top = false, bottom = false;
    };
    uint64_t id = 0;
    SpanId span;
    int64_t tickUnits = 0;
    double tick = 0;
    std::array<uint64_t, kMaxSpanSources> passes{};
    uint8_t passCount = 0;
    BinGrid grid; // all rows (tfMs, firstBucket, 64 columns, tick, firstBin, rows)
    std::vector<Block> blocks;
    uint32_t usedBlocks = 0;
    uint64_t usedFrame = 0, drawnFrame = 0, pinnedFrame = 0;
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

void HeatmapTileNode::releaseAll(bool reportLoss, bool inRhiCleanup) {
    bool held = !bins_.empty();
    for (const auto &[key, s] : sources_) held = held || s->created;
    bins_.clear();
    spareBuffers_.clear();
    sources_.clear();
    byId_.clear();
    indexedSet_.reset();
    indexed_ = nullptr;
    if (inRhiCleanup) {
        // The binner owns a cleanup callback of its own and releases its GPU
        // resources there; destroying it here would remove that callback while
        // the QRhi iterates them. It goes after the traversal.
        if (binner_) retiredBinner_ = std::move(binner_);
    } else {
        binner_.reset(); // completes its own in-flight readbacks first
        retiredBinner_.reset();
    }
    pipeline_.reset(); // before the loading draws: it was created with the first one's bindings
    loadingDraws_.clear();
    loadingCell_.reset();
    held_.clear();
    now_.clear();
    for (auto &layer : fading_) { layer.draws.clear(); layer.used = false; }
    tileDraws_.clear();
    drawnTf_ = drawnTick_ = 0;
    reindex_ = true;
    uploaded_.clear();
    reportedMissing_.clear();
    stats_->complete.store(false);
    stats_->residentSources.store(0);
    {
        std::scoped_lock lock(stats_->mutex);
        stats_->resident.clear();
        stats_->drawn.clear();
    }
    if (reportLoss && held) {
        // The GPU copies are gone: the controller rebuilds the released images.
        lost_ = true;
        if (frame_.capacity) {
            frame_.capacity->report(frame_.gpuCapBytes, {}, true);
            stats_->reports.fetch_add(1);
            lost_ = false;
            reportedTo_ = frame_.capacity;
            reportedBytes_ = 0;
        }
        sLog_Render("Heatmap tile node lost its GPU content (QRhi released); reported to the controller");
    }
}

void HeatmapTileNode::releaseResources() {
    if (rhi_) rhi_->removeCleanupCallback(this);
    releaseAll(true);
    rhi_ = nullptr;
}

void HeatmapTileNode::attachRhi(QRhi *rhi) {
    if (rhi_) rhi_->removeCleanupCallback(this);
    releaseAll(true);
    rhi_ = rhi;
    // Scene graph invalidation can destroy the QRhi before this node: free every
    // resource while it still works, and tell the controller. Only GPU resources
    // go here; objects that own cleanup callbacks outlive the traversal.
    rhi_->addCleanupCallback(this, [this](QRhi *) {
        HeatmapGpuBinner::RhiCleanupScope scope;
        releaseAll(true, true);
        rhi_ = nullptr;
    });
    binner_ = std::make_unique<HeatmapGpuBinner>(rhi_);
}

void HeatmapTileNode::noteError(const QString &error) {
    stats_->errors.fetch_add(1);
    if (error != lastError_) sLog_Warning("heatmap tile node: " << error);
    lastError_ = error;
}

// ------------------------------------------------------------------ snapshot index
void HeatmapTileNode::reindex() {
    indexedSet_ = frame_.spans;
    const SpanSet *set = indexedSet_.get();
    spanRefs_.clear();
    byTile_.clear();
    indexed_ = set;
    indexedVersion_ = set ? set->version : 0;
    reindex_ = false;
    if (!set) return;
    const uint64_t version = set->version;
    spanRefs_.reserve(set->spans.size());
    for (const auto &span : set->spans) {
        SpanRef ref;
        ref.span = &span;
        ref.count = uint8_t(std::min(span.sources.size(), kMaxSpanSources));
        for (size_t i = 0; i < ref.count; ++i) {
            const auto &snap = span.sources[i];
            if (!snap.build) continue;
            auto &entry = sources_[snap.build->key];
            if (!entry) {
                entry = std::make_unique<Source>();
                entry->key = snap.build->key;
                // Union of this source's full-coverage bands over the span.
                bool any = false;
                for (const auto &column : snap.build->resolution.columns)
                    for (const auto &source : column.sources)
                        for (const auto &band : source.bands) {
                            entry->bandLo = any ? std::min(entry->bandLo, band.lo) : band.lo;
                            entry->bandEnd = any ? std::max(entry->bandEnd, band.end) : band.end;
                            any = true;
                        }
            }
            // A newer publication of the same key (the controller released the
            // image, or rebuilt it after a loss) replaces the metadata.
            entry->build = snap.build;
            entry->listedVersion = version;
            entry->rank = span.rank;
            ref.sources[i] = entry.get();
        }
        byTile_.emplace_back(span.id, spanRefs_.size());
        spanRefs_.push_back(ref);
    }
    std::sort(byTile_.begin(), byTile_.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    // Released images the node does not hold may be reported missing again.
    std::erase_if(reportedMissing_, [&](const SpanSourceKey &key) {
        const auto it = sources_.find(key);
        return it == sources_.end() || it->second->listedVersion != version || !it->second->build ||
               it->second->build->gpu;
    });
}

const HeatmapTileNode::SpanRef *HeatmapTileNode::spanAt(int64_t tfMs, int64_t tile) const {
    if (!indexed_) return nullptr;
    const SpanId id{indexed_->symbol, tfMs, tile};
    const auto it = std::lower_bound(byTile_.begin(), byTile_.end(), id,
                                     [](const auto &entry, const SpanId &key) { return entry.first < key; });
    return it != byTile_.end() && it->first == id ? &spanRefs_[it->second] : nullptr;
}

// The first source of a span is always wanted; a finer one while the rows around
// the view overlap its bands (a refinement to its tick then draws next frame).
bool HeatmapTileNode::wanted(const SpanRef &ref, size_t index) const {
    const Source *source = ref.sources[index];
    if (!source) return false;
    size_t first = 0;
    while (first < ref.count && !ref.sources[first]) ++first;
    if (index == first) return true;
    if (source->bandEnd <= source->bandLo || !indexed_) return false;
    const auto &v = frame_.view;
    const double span = v.priceHi - v.priceLo, scale = indexed_->priceScale;
    const double lo = (v.priceLo - span) * scale, hi = (v.priceHi + span) * scale;
    return double(source->bandLo) < hi && double(source->bandEnd) > lo;
}

// ------------------------------------------------------------------ residency
void HeatmapTileNode::freeSource(Source &source, bool report) {
    if (source.created && source.uploadFrame == frameNo_) stats_->sameFrameReleases.fetch_add(1);
    if (source.created) {
        binner_->releaseResident(source.sourceId);
        byId_.erase(source.sourceId);
        stats_->evictions.fetch_add(report ? 1 : 0);
    }
    const bool listed = indexed_ && source.listedVersion == indexedVersion_;
    if (report && listed && source.reportedUploaded) {
        // The controller released the image because we had it: it must rebuild.
        const auto pending = std::find(uploaded_.begin(), uploaded_.end(), source.key);
        if (pending != uploaded_.end()) uploaded_.erase(pending); // never reported: nothing to undo
        else {
            missing_.push_back(source.key);
            reportedMissing_.push_back(source.key);
        }
    }
    source.created = source.complete = source.reportedUploaded = false;
    source.sourceId = 0;
    source.bytes = 0;
}

// Evicts unpinned sources (never visible, fallback or drawn) until resident bytes
// are at most `target`. With `incoming`, only content ranked strictly below it.
void HeatmapTileNode::evictDown(uint64_t target, const SpanRank *incoming) {
    // Retired bin buffers kept for reuse go first.
    while (!spareBuffers_.empty() && residentBytes() > target) spareBuffers_.pop_back();
    while (residentBytes() > target) {
        Source *victim = nullptr;
        std::pair<int, int64_t> worst{0, 0};
        for (auto &[key, entry] : sources_) {
            auto &s = *entry;
            // Never what this frame drew or uploaded into: its commands reference it.
            if (!s.created || s.pinnedFrame == frameNo_ || s.uploadFrame == frameNo_) continue;
            const bool listed = indexed_ && s.listedVersion == indexedVersion_;
            if (listed && guarded(s.rank.tier)) continue;
            if (incoming && listed && !(*incoming < s.rank)) continue;
            const auto order = evictionOrder(listed, s.rank);
            if (!victim || order > worst) {
                victim = &s;
                worst = order;
            }
        }
        if (!victim) return;
        sLog_Probe("heatmap.node.evict", "tile=" << victim->key.span.tile << " tf=" << victim->key.span.tfMs
                   << " source=" << victim->key.source << " tier=" << spanTierName(victim->rank.tier)
                   << " bytes=" << victim->bytes);
        freeSource(*victim, true);
    }
}

bool HeatmapTileNode::admit(Source &source) {
    const uint64_t need = source.build->uploadBytes + kSourceOverheadBytes, cap = frame_.gpuCapBytes;
    if (residentBytes() + need <= cap) return true;
    evictDown(cap > need ? cap - need : 0, &source.rank);
    if (residentBytes() + need <= cap) return true;
    const bool listed = indexed_ && source.listedVersion == indexedVersion_;
    return listed && guarded(source.rank.tier); // visible and fallback content may exceed the cap
}

void HeatmapTileNode::upload(QRhiCommandBuffer *cb) {
    uint64_t budget = std::max<uint64_t>(frame_.uploadBudgetBytes, 1);
    // Rank order (the snapshot is sorted by rank): visible, fallback, prefetch near
    // to far, recent-tf.
    order_.clear();
    for (const auto &ref : spanRefs_)
        for (size_t i = 0; i < ref.count; ++i)
            if (ref.sources[i] && !ref.sources[i]->complete && wanted(ref, i)) order_.push_back(ref.sources[i]);
    for (Source *s : order_) {
        if (!budget) break;
        const auto &image = s->build->gpu;
        if (!image) continue; // released but not held: reported missing
        if (!s->created && !admit(*s)) continue;
        bool complete = false;
        const uint64_t before = budget;
        QString error;
        if (!binner_->uploadResident(image, cb, budget, &complete, &error)) {
            noteError(error);
            freeSource(*s, false);
            continue;
        }
        s->uploadFrame = frameNo_;
        if (!s->created) {
            s->created = true;
            s->sourceId = image->id;
            s->bytes = binner_->residentBytes(image->id);
            byId_[s->sourceId] = s;
        }
        stats_->uploadBytes.fetch_add(before - budget);
        if (complete) {
            s->complete = true;
            s->reportedUploaded = true;
            uploaded_.push_back(s->key);
            stats_->sourcesUploaded.fetch_add(1);
            sLog_Probe("heatmap.node.upload", "tf=" << s->key.span.tfMs << " tile=" << s->key.span.tile << " source="
                       << s->key.source << " bytes=" << s->bytes << " tier=" << spanTierName(s->rank.tier));
        }
    }
}

// ------------------------------------------------------------------ bins
std::unique_ptr<QRhiBuffer> HeatmapTileNode::cellBuffer(uint64_t bytes) {
    bytes = (bytes + 15) / 16 * 16;
    // The smallest retired buffer that fits, else a new one.
    auto best = spareBuffers_.end();
    for (auto it = spareBuffers_.begin(); it != spareBuffers_.end(); ++it)
        if ((*it)->size() >= bytes && (*it)->size() <= bytes * 2 &&
            (best == spareBuffers_.end() || (*it)->size() < (*best)->size()))
            best = it;
    if (best != spareBuffers_.end()) {
        auto buffer = std::move(*best);
        spareBuffers_.erase(best);
        return buffer;
    }
    std::unique_ptr<QRhiBuffer> buffer(rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32(bytes)));
    buffer->setName("heatmap.tileCells");
    if (!buffer->create()) {
        noteError(QStringLiteral("tile cell buffer allocation failed (%1 bytes)").arg(bytes));
        return {};
    }
    return buffer;
}

bool HeatmapTileNode::rowsCover(const Bin &bin) const {
    const double tick = bin.tick;
    const auto first = int64_t(std::floor(frame_.view.priceLo / tick)) - 1;
    const auto end = int64_t(std::ceil(frame_.view.priceHi / tick)) + 1;
    return first >= bin.grid.firstBin && end <= bin.grid.firstBin + int64_t(bin.grid.rows);
}

// Bins the rows around the view (one view height each side) into the bin's
// blocks: the first source, then each finer source as a fill pass.
bool HeatmapTileNode::binRows(Bin &bin, QRhiCommandBuffer *cb) {
    const auto started = std::chrono::steady_clock::now();
    const auto &view = frame_.view;
    const double span = view.priceHi - view.priceLo;
    bin.grid.firstBin = int64_t(std::floor((view.priceLo - span) / bin.tick)) - 1;
    const int64_t end = int64_t(std::ceil((view.priceHi + span) / bin.tick)) + 1;
    bin.grid.rows = uint32_t(std::clamp<int64_t>(end - bin.grid.firstBin, 1, kMaxRows));
    const uint32_t blocks = (bin.grid.rows + kBlockRows - 1) / kBlockRows;
    if (bin.blocks.size() < blocks) bin.blocks.resize(blocks);
    bin.usedBlocks = blocks;
    QString error;
    const auto fs = QRhiShaderResourceBinding::FragmentStage, vs = QRhiShaderResourceBinding::VertexStage;
    for (uint32_t b = 0; b < blocks; ++b) {
        auto &block = bin.blocks[b];
        const uint32_t top = b * kBlockRows;
        block.grid = bin.grid;
        block.grid.rows = std::min(kBlockRows, bin.grid.rows - top);
        block.grid.firstBin = bin.grid.firstBin + int64_t(bin.grid.rows - top - block.grid.rows);
        block.top = b == 0;
        block.bottom = b + 1 == blocks;
        const uint64_t bytes = uint64_t(block.grid.columns) * block.grid.rows * 4;
        if (!block.cells || block.cells->size() < bytes) {
            if (block.cells && spareBuffers_.size() < kMaxSpareBuffers) spareBuffers_.push_back(std::move(block.cells));
            block.cells = cellBuffer(bytes);
            if (!block.cells) return false;
        }
        if (!block.params) {
            block.params.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(DrawParams)));
            if (!block.params->create()) {
                block.params.reset();
                noteError(QStringLiteral("tile uniform allocation failed"));
                return false;
            }
        }
        if (!block.bindings || block.boundTo != block.cells.get()) {
            if (!block.bindings) block.bindings.reset(rhi_->newShaderResourceBindings());
            block.bindings->setBindings({QRhiShaderResourceBinding::bufferLoad(0, fs, block.cells.get()),
                                         QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, block.params.get())});
            if (!block.bindings->create()) { noteError(QStringLiteral("tile bindings failed")); return false; }
            block.boundTo = block.cells.get();
        }
        for (uint8_t p = 0; p < bin.passCount; ++p) {
            if (!binner_->binResidentInto(bin.passes[p], cb, block.grid, frame_.outputScale, block.cells.get(),
                                          &error, p > 0)) {
                noteError(error);
                return false;
            }
            stats_->binPasses.fetch_add(1);
            if (p > 0) stats_->fillPasses.fetch_add(1);
        }
    }
    // Blocks beyond the rows in use keep their buffers for the next re-bin.
    stats_->lastBinMs.store(msSince(started));
    stats_->preciseKernel.store(binner_->currentKernel() == KernelVariant::Precise);
    return true;
}

HeatmapTileNode::Bin *HeatmapTileNode::findBin(uint64_t id) const {
    for (const auto &bin : bins_)
        if (bin->id == id) return bin.get();
    return nullptr;
}

// The bin of `ref` at the tick from its resident wanted sources (in fill order),
// made or re-binned now if needed. *complete: every built source it wants is in it.
HeatmapTileNode::Bin *HeatmapTileNode::binFor(const SpanRef &ref, int64_t tickUnits, QRhiCommandBuffer *cb,
                                              bool *complete) {
    std::array<uint64_t, kMaxSpanSources> passes{};
    uint8_t count = 0;
    *complete = true;
    for (size_t i = 0; i < ref.count; ++i) {
        const auto &snap = ref.span->sources[i];
        const Source *source = ref.sources[i];
        if (!snap.build || !source) {
            if (!snap.failed) *complete = false; // still building
            continue;
        }
        if (!wanted(ref, i)) continue;
        if (!source->complete) {
            *complete = false;
            continue;
        }
        passes[count++] = source->sourceId;
    }
    if (!count || tickUnits <= 0 || !indexed_) return nullptr;
    for (const auto &bin : bins_) {
        if (bin->span != ref.span->id || bin->tickUnits != tickUnits || bin->passCount != count ||
            !std::equal(passes.begin(), passes.begin() + count, bin->passes.begin()))
            continue;
        bin->usedFrame = frameNo_;
        if (!rowsCover(*bin)) {
            if (binRows(*bin, cb)) stats_->rebins.fetch_add(1);
            else *complete = false;
        }
        return bin.get();
    }
    auto bin = std::make_unique<Bin>();
    bin->id = nextBinId_++;
    bin->span = ref.span->id;
    bin->tickUnits = tickUnits;
    bin->tick = fromUnits(tickUnits, indexed_->priceScale);
    bin->passes = passes;
    bin->passCount = count;
    bin->grid.tfMs = ref.span->id.tfMs;
    bin->grid.firstBucket = tiles::tileFirstBucket(ref.span->id.tile);
    bin->grid.columns = uint32_t(tiles::kTileColumns);
    bin->grid.displayTick = bin->tick;
    bin->usedFrame = frameNo_;
    if (!binRows(*bin, cb)) {
        *complete = false;
        return nullptr;
    }
    stats_->binsMade.fetch_add(1);
    sLog_Probe("heatmap.node.bin", "tf=" << bin->span.tfMs << " tile=" << bin->span.tile << " tick=" << tickUnits
               << " passes=" << int(count) << " rows=" << bin->grid.rows << " ms=" << stats_->lastBinMs.load());
    bins_.push_back(std::move(bin));
    return bins_.back().get();
}

uint64_t HeatmapTileNode::spareBytes() const {
    uint64_t total = 0;
    for (const auto &b : spareBuffers_) total += b->size();
    return total;
}
uint64_t HeatmapTileNode::residentBytes() const {
    uint64_t total = spareBytes();
    for (const auto &[id, s] : byId_) total += s->bytes;
    for (const auto &bin : bins_) total += bin->bytes();
    return total;
}

void HeatmapTileNode::pinSources(const Bin &bin) {
    for (uint8_t p = 0; p < bin.passCount; ++p)
        if (const auto it = byId_.find(bin.passes[p]); it != byId_.end()) it->second->pinnedFrame = frameNo_;
}

// Frees bins nothing drew or used this frame and sources the snapshot no longer
// lists and nothing drawn needs. keepDrawn = false (retention disabled, tests):
// drawn content the current target does not use is freed too, as the B1 node did.
void HeatmapTileNode::retire(bool keepDrawn) {
    auto pin = [&](const Draw &d) {
        if (Bin *bin = findBin(d.bin)) {
            bin->pinnedFrame = frameNo_;
            pinSources(*bin);
        }
    };
    if (keepDrawn) {
        for (const auto &d : now_) pin(d);
        for (const auto &d : held_) pin(d);
        for (const auto &layer : fading_)
            for (const auto &d : layer.draws) pin(d);
    }
    for (auto it = bins_.begin(); it != bins_.end();) {
        auto &bin = **it;
        if (bin.pinnedFrame == frameNo_ || bin.usedFrame == frameNo_) {
            pinSources(bin); // a current bin may re-bin next frame
            ++it;
            continue;
        }
        for (auto &block : bin.blocks)
            if (block.cells && spareBuffers_.size() < kMaxSpareBuffers) spareBuffers_.push_back(std::move(block.cells));
        it = bins_.erase(it);
    }
    for (auto it = sources_.begin(); it != sources_.end();) {
        auto &s = *it->second;
        const bool listed = indexed_ && s.listedVersion == indexedVersion_;
        if (listed || s.pinnedFrame == frameNo_ || s.uploadFrame == frameNo_) { ++it; continue; }
        freeSource(s, false);
        it = sources_.erase(it);
    }
    // Over the cap: evict what nothing drawn needs (recent-tf, far prefetch first).
    if (residentBytes() > frame_.gpuCapBytes) {
        evictDown(frame_.gpuCapBytes, nullptr);
        if (const uint64_t resident = residentBytes(); resident > frame_.gpuCapBytes)
            sLog_RenderN(5000, "Heatmap node over its GPU cap with content it may not evict (visible, fallback, drawn)"
                               " resident=" << resident << " cap=" << frame_.gpuCapBytes << " sources=" << byId_.size()
                               << " bins=" << bins_.size());
    }
}

void HeatmapTileNode::report() {
    // Released images the node does not hold (a new node, or content lost).
    if (indexed_)
        for (const auto &ref : spanRefs_)
            for (size_t i = 0; i < ref.count; ++i) {
                const Source *s = ref.sources[i];
                if (!s || !s->build || s->build->gpu || s->complete) continue;
                if (std::find(reportedMissing_.begin(), reportedMissing_.end(), s->key) != reportedMissing_.end()) continue;
                missing_.push_back(s->key);
                reportedMissing_.push_back(s->key);
            }
    const auto &capacity = frame_.capacity;
    if (!capacity) {
        uploaded_.clear();
        missing_.clear();
        return;
    }
    if (capacity != reportedTo_) { // a new controller: tell it where we stand
        reportedTo_ = capacity;
        reportedBytes_ = UINT64_MAX;
    }
    const uint64_t resident = residentBytes(), cap = frame_.gpuCapBytes;
    // A new cap is new free bytes, even when nothing moved (suppressed prefetch).
    if (resident == reportedBytes_ && cap == reportedCap_ && uploaded_.empty() && missing_.empty() && !lost_) return;
    stats_->missingReports.fetch_add(missing_.size());
    capacity->report(size_t(cap > resident ? cap - resident : 0), std::move(uploaded_), lost_, std::move(missing_));
    stats_->reports.fetch_add(1);
    uploaded_.clear();
    missing_.clear();
    lost_ = false;
    reportedBytes_ = resident;
    reportedCap_ = cap;
}

// ------------------------------------------------------------------ drawing
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

void HeatmapTileNode::addBinDraw(QRhiResourceUpdateBatch *updates, Bin &bin, float opacity) {
    if (bin.drawnFrame == frameNo_) return; // one uniform block per bin and frame
    QRectF r;
    ViewWindow sub;
    const int64_t first = tiles::tileFirstBucket(bin.span.tile);
    if (!subRect(first, first + tiles::kTileColumns, bin.span.tfMs, &r, &sub)) return;
    const auto &view = frame_.view;
    const double priceSpan = view.priceHi - view.priceLo;
    if (!(priceSpan > 0)) return;
    const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
    bool any = false;
    for (uint32_t b = 0; b < bin.usedBlocks; ++b) {
        auto &block = bin.blocks[b];
        // Price rows of this block; the outer blocks extend to the view edges,
        // where the clamped sentinel rows repeat each column's outside state.
        const double lo = block.bottom ? view.priceLo : std::max(view.priceLo, double(block.grid.firstBin) * bin.tick);
        const double hi = block.top ? view.priceHi
                                    : std::min(view.priceHi, double(block.grid.firstBin + int64_t(block.grid.rows)) * bin.tick);
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
    if (any) bin.drawnFrame = frameNo_;
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

// The loading hatch over the part of [loMs, hiMs) in view; false when none is.
bool HeatmapTileNode::addLoadingDraw(QRhiResourceUpdateBatch *updates, int64_t loMs, int64_t hiMs) {
    QRectF r;
    ViewWindow sub;
    if (!subRect(loMs, hiMs, 1, &r, &sub)) return false;
    if (loadingUsed_ == loadingDraws_.size() && !addLoadingSlot()) return false;
    DrawParams p{};
    const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
    std::memcpy(p.mvp, mvp.constData(), sizeof(p.mvp));
    p.rect[0] = float(r.x()); p.rect[1] = float(r.y()); p.rect[2] = float(r.width()); p.rect[3] = float(r.height());
    p.mapping[0] = 0; p.mapping[1] = 0.999f; p.mapping[2] = 0; p.mapping[3] = 1;
    p.dims[0] = 1; p.dims[1] = 1; p.dims[2] = kClampRows;
    p.style[0] = frame_.style.codeFloor; p.style[1] = frame_.style.codeRange; p.style[2] = frame_.style.opacity;
    updates->updateDynamicBuffer(loadingDraws_[loadingUsed_]->params.get(), 0, sizeof(p), &p);
    ++loadingUsed_;
    return true;
}

// ------------------------------------------------------------------ frame
void HeatmapTileNode::prepare() {
    const auto started = std::chrono::steady_clock::now();
    tileDraws_.clear();
    loadingUsed_ = 0;
    QRhiCommandBuffer *cb = commandBuffer();
    QRhiRenderTarget *rt = renderTarget();
    if (!cb || !rt) return;
    QRhi *rhi = rt->rhi();
    retiredBinner_.reset(); // its QRhi's cleanup traversal is long over
    if (rhi != rhi_) attachRhi(rhi);
    ++frameNo_;
    stats_->frames.fetch_add(1);
    if (!binner_) binner_ = std::make_unique<HeatmapGpuBinner>(rhi_);
    binner_->runPrecisionSelfTest(cb);
    if (!loadingCell_) {
        loadingCell_.reset(rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, 16));
        if (!loadingCell_->create()) return noteError(QStringLiteral("loading cell allocation failed"));
        const uint32_t cell = uint32_t(CellState::Loading) << 16;
        auto *updates = rhi_->nextResourceUpdateBatch();
        updates->uploadStaticBuffer(loadingCell_.get(), 0, 4, &cell);
        cb->resourceUpdate(updates);
        // The graphics pipeline takes its binding layout from the first loading
        // slot, so it exists before any draw needs it (FM-102).
        if (!addLoadingSlot()) return;
    }
    const SpanSet *set = frame_.spans.get();
    if (reindex_ || frame_.spans != indexedSet_) reindex();
    const bool keepDrawn = !retentionDisabled.load();

    // 1. Residency: pins from the last picture, then uploads by rank.
    if (keepDrawn) {
        auto pinDraw = [&](const Draw &d) {
            if (Bin *bin = findBin(d.bin)) pinSources(*bin);
        };
        for (const auto &d : held_) pinDraw(d);
        for (const auto &layer : fading_)
            for (const auto &d : layer.draws) pinDraw(d);
    }
    // The cap first, before this frame records anything into the pool: what the
    // last frame's bins and uploads pushed over it goes now (nothing of this
    // frame references it yet).
    if (residentBytes() > frame_.gpuCapBytes) evictDown(frame_.gpuCapBytes, nullptr);
    upload(cb);

    // 2. The target's visible spans: bin them at the tick (same frame).
    const int64_t tf = frame_.tfMs, tick = frame_.tickUnits;
    slots_.clear();
    uint32_t refusedSlots = 0;
    if (set && tf > 0) {
        auto range = tiles::tilesCovering(frame_.view.timeLoMs, frame_.view.timeHiMs, tf, 0);
        if (range.count() > 4096) range.end = range.first; // degenerate view (time zoom is clamped upstream)
        for (int64_t t = range.first; t < range.end; ++t) {
            Slot s;
            s.tile = t;
            s.expected = set->availableEndMs > set->availableStartMs && tiles::tileEndMs(t, tf) > set->availableStartMs &&
                         tiles::tileStartMs(t, tf) < set->availableEndMs;
            const SpanId id{set->symbol, tf, t};
            s.refused = std::find(set->refused.begin(), set->refused.end(), id) != set->refused.end();
            if (const SpanRef *ref = spanAt(tf, t); ref && tick > 0) {
                bool complete = false;
                s.bin = binFor(*ref, tick, cb, &complete);
                // Complete without a bin: every source failed terminally. That is
                // resolved (it draws loading), not pending: it must not hold a
                // transition forever.
                s.ready = complete;
                s.complete = s.bin && complete && ref->span->complete;
            }
            // Refused spans draw loading until capacity frees; time outside the
            // data draws nothing: neither waits for anything.
            if (s.refused || !s.expected) s.ready = s.complete = true;
            refusedSlots += s.refused;
            slots_.push_back(s);
        }
    }

    // 3. Transitions.
    const bool targetChanged = tf != drawnTf_ || tick != drawnTick_;
    bool allReady = tick > 0 && !slots_.empty();
    for (const auto &s : slots_) allReady = allReady && s.ready;
    bool holding = false;
    if (targetChanged) {
        if (allReady) {
            if (drawnTf_ == tf && drawnTick_ > 0 && frame_.crossfadeMs > 0 && !held_.empty()) {
                // The last picture fades out over the new one (spec rule 8). An
                // earlier fading layer finishes its own fade.
                Layer *layer = nullptr;
                for (auto &l : fading_) if (!l.used) { layer = &l; break; }
                if (!layer) { // all layers busy: the oldest one ends now
                    layer = &*std::min_element(fading_.begin(), fading_.end(),
                                               [](const Layer &a, const Layer &b) { return a.start < b.start; });
                }
                layer->draws.assign(held_.begin(), held_.end());
                layer->start = std::chrono::steady_clock::now();
                layer->used = true;
            }
            sLog_Probe("heatmap.node.switch", "tf=" << drawnTf_ << "->" << tf << " tick=" << drawnTick_ << "->" << tick
                       << " slots=" << slots_.size());
            drawnTf_ = tf;
            drawnTick_ = tick;
        } else {
            holding = !held_.empty();
            if (!holding) { // nothing drawn yet: show the target as it arrives
                drawnTf_ = tf;
                drawnTick_ = tick;
            }
        }
    }
    // Retention disabled (tests): drawn content the target does not use goes now,
    // before it is drawn, as the B1 node freed tiles the controller dropped.
    if (!keepDrawn) retire(false);
    auto *updates = rhi_->nextResourceUpdateBatch();
    uint32_t ready = 0, fallback = 0, partial = 0, loading = 0;
    uint64_t missingDraws = 0, unpinnedDraws = 0;
    now_.clear();
    auto drawBin = [&](const Draw &d, float opacity) {
        Bin *bin = findBin(d.bin);
        if (!bin) { ++missingDraws; return; }
        if (!rowsCover(*bin) && binRows(*bin, cb)) stats_->rebins.fetch_add(1);
        addBinDraw(updates, *bin, opacity);
    };
    // Loading over the parts of [lo, hi) no drawn picture covers (holding across
    // a tf change: the old tiles are narrower or wider than the new slots).
    auto &covered = covered_;
    covered.clear();
    auto hatch = [&](int64_t lo, int64_t hi) {
        lo = std::max(lo, set ? set->availableStartMs : lo);
        hi = std::min(hi, set ? set->availableEndMs : hi);
        for (const auto &[a, b] : covered) {
            if (b <= lo || a >= hi) continue;
            if (a > lo) loading += addLoadingDraw(updates, lo, a);
            lo = std::max(lo, b);
        }
        if (hi > lo) loading += addLoadingDraw(updates, lo, hi);
    };
    if (holding) {
        now_.assign(held_.begin(), held_.end());
        for (const auto &d : held_) {
            drawBin(d, 1.0f);
            covered.emplace_back(tiles::tileStartMs(d.tile, d.tfMs), tiles::tileEndMs(d.tile, d.tfMs));
        }
        std::sort(covered.begin(), covered.end());
        for (const auto &s : slots_)
            if (s.expected) hatch(tiles::tileStartMs(s.tile, tf), tiles::tileEndMs(s.tile, tf));
    } else {
        for (const auto &s : slots_) {
            const Draw *previous = nullptr;
            if (!s.ready)
                for (const auto &d : held_)
                    if (d.tile == s.tile && d.tfMs == tf) { previous = &d; break; }
            if (s.ready && s.bin) {
                now_.push_back({s.bin->id, s.tile, tf});
                ++ready;
            } else if (previous) {
                now_.push_back(*previous); // slot fallback: the previous content until the new one is ready
                ++fallback;
            } else if (s.bin) {
                now_.push_back({s.bin->id, s.tile, tf}); // what is resident so far
                ++partial;
            } else if (s.expected) {
                hatch(tiles::tileStartMs(s.tile, tf), tiles::tileEndMs(s.tile, tf));
            }
        }
        for (const auto &d : now_) drawBin(d, 1.0f);
        held_.assign(now_.begin(), now_.end()); // the newest picture of this target
    }
    // Fading layers, oldest first, over the current picture.
    fadeDrawsFrom_ = tileDraws_.size();
    uint32_t fadingLayers = 0;
    std::array<const Layer *, kMaxFadingLayers> byAge{};
    for (size_t i = 0; i < fading_.size(); ++i) byAge[i] = &fading_[i];
    std::sort(byAge.begin(), byAge.end(), [](const Layer *a, const Layer *b) { return a->start < b->start; });
    for (const Layer *l : byAge) {
        auto &layer = const_cast<Layer &>(*l);
        if (!layer.used) continue;
        const double t = msSince(layer.start) / std::max(frame_.crossfadeMs, 1e-3);
        if (t >= 1 || frame_.crossfadeMs <= 0) {
            layer.used = false;
            layer.draws.clear();
            continue;
        }
        ++fadingLayers;
        for (const auto &d : layer.draws) drawBin(d, float(1 - t));
    }
    cb->resourceUpdate(updates);
    if (!ensurePipeline(rt->renderPassDescriptor(), rt->sampleCount()) && !loadingDraws_.empty())
        noteError(QStringLiteral("heatmap tile pipeline failed"));

    // 4. Retire what nothing needs, enforce the cap, report.
    retire(keepDrawn);
    // Retention check (stats): every drawn bin is resident, with its sources.
    auto &drawn = drawnScratch_;
    drawn.clear();
    auto check = [&](const Draw &d) {
        drawn.push_back(d.bin);
        const Bin *bin = findBin(d.bin);
        if (!bin) { ++missingDraws; return; }
        for (uint8_t p = 0; p < bin->passCount; ++p)
            if (!binner_->isResident(bin->passes[p])) ++unpinnedDraws;
    };
    for (const auto &d : now_) check(d);
    for (const auto &layer : fading_)
        if (layer.used)
            for (const auto &d : layer.draws) check(d);
    report();

    uint64_t sourceBytes = 0, binBytes = 0;
    for (const auto &[id, s] : byId_) sourceBytes += s->bytes;
    for (const auto &bin : bins_) binBytes += bin->bytes();
    const uint64_t resident = residentBytes();
    {
        std::scoped_lock lock(stats_->mutex);
        stats_->drawn.assign(drawn.begin(), drawn.end());
        stats_->resident.clear();
        for (const auto &bin : bins_) stats_->resident.push_back(bin->id);
    }
    bool complete = !holding && tick > 0 && !slots_.empty() && tf == drawnTf_ && tick == drawnTick_;
    for (const auto &s : slots_) complete = complete && s.complete;
    complete = complete && !fallback && !partial;
    stats_->missingDraws.fetch_add(missingDraws);
    stats_->unpinnedDraws.fetch_add(unpinnedDraws);
    stats_->residentSources.store(byId_.size());
    stats_->sourceBytes.store(sourceBytes);
    stats_->binBytes.store(binBytes);
    stats_->residentBytes.store(resident);
    uint64_t fixed = binner_->gpuBytes() - binner_->residentBytes() + (loadingCell_ ? loadingCell_->size() : 0);
    for (const auto &slot : loadingDraws_) fixed += slot->params ? slot->params->size() : 0;
    stats_->gpuBytes.store(resident + fixed);
    stats_->slotCount.store(uint32_t(slots_.size()));
    stats_->readySlots.store(ready);
    stats_->fallbackSlots.store(fallback);
    stats_->partialSlots.store(partial);
    stats_->loadingSlots.store(loading);
    stats_->refusedSlots.store(refusedSlots);
    stats_->fadingLayers.store(fadingLayers);
    stats_->holding.store(holding);
    stats_->crossfading.store(fadingLayers > 0);
    stats_->drawnTickUnits.store(drawnTick_);
    stats_->drawnTfMs.store(drawnTf_);
    stats_->complete.store(complete);
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
    // The current picture, then loading hatches, then the fading layers over them.
    for (size_t i = 0; i < fadeDrawsFrom_ && i < tileDraws_.size(); ++i) {
        cb->setShaderResources(tileDraws_[i].first);
        cb->draw(4);
    }
    for (size_t i = 0; i < loadingUsed_; ++i) {
        cb->setShaderResources(loadingDraws_[i]->bindings.get());
        cb->draw(4);
    }
    for (size_t i = fadeDrawsFrom_; i < tileDraws_.size(); ++i) {
        cb->setShaderResources(tileDraws_[i].first);
        cb->draw(4);
    }
}
} // namespace heatmap::gpu
