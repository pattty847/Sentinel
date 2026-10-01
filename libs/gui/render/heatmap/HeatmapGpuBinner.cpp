#include "HeatmapGpuBinner.hpp"
#include "HeatmapGpuSelfTest.hpp"
#include "SentinelLogging.hpp"
#include <QFile>
#include <QVarLengthArray>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace heatmap::gpu {
namespace {
QShader loadShader(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
// name: debug name for graphics debuggers and validation layers (set before create()).
std::unique_ptr<QRhiBuffer> makeBuffer(QRhi *rhi, QRhiBuffer::Type type, QRhiBuffer::UsageFlags usage,
                                       uint64_t bytes, QString *error, const QByteArray &name = {}) {
    bytes = std::max<uint64_t>((bytes + 15) / 16 * 16, 16);
    if (bytes > std::numeric_limits<quint32>::max()) {
        fail(error, QStringLiteral("GPU buffer exceeds 4 GiB"));
        return {};
    }
    std::unique_ptr<QRhiBuffer> buffer(rhi->newBuffer(type, usage, quint32(bytes)));
    if (buffer) buffer->setName(name);
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
    std::array<float, 4> codeScale;
    std::array<uint32_t, 4> paging;
};
static_assert(sizeof(ComputeParams) == 144);
constexpr uint64_t kSmallAllocationBytes = 1ull << 20; // buffers this small are created together
const char *kPreciseShader = ":/heatmapgpu/heatmap_bin_precise.comp.qsb";

int32_t clampToInt32(double value, double lo, double hi) {
    if (std::isnan(value)) return 0;
    return int32_t(std::clamp(value, lo, hi));
}
bool sameScale(const recording::SizeScale &a, const recording::SizeScale &b) {
    return a.floor == b.floor && a.codesPerOctave == b.codesPerOctave;
}
// Self-test verdict per backend/device, shared by every binner in the process.
std::mutex selfTestMutex;
std::map<QString, KernelVariant> selfTestCache;
std::atomic<uint64_t> drainedReadbacks{0}; // tests: see drainedReadbacksForTest
thread_local int rhiCleanupDepth = 0;       // inside a QRhi cleanup-callback traversal
std::atomic<uint64_t> removalsDuringCleanup{0};
} // namespace

HeatmapGpuBinner::RhiCleanupScope::RhiCleanupScope() { ++rhiCleanupDepth; }
HeatmapGpuBinner::RhiCleanupScope::~RhiCleanupScope() { --rhiCleanupDepth; }
uint64_t HeatmapGpuBinner::callbackRemovalsDuringCleanupForTest() { return removalsDuringCleanup.load(); }

// What a bin needs of a source: resident sources keep only this once their
// upload completed, so the CPU image can be released (S5 capacity contract).
struct SourceParams {
    int64_t firstBucket = 0, availableFirstBucket = 0, availableEndBucket = 0, tfMs = 0;
    double clipPriceLo = 0, clipPriceHi = 0;
    std::vector<double> ticks;
    uint32_t slotCount = 0, pageShift = 0, wordsPerEntry = 2;
    bool wide = false;
    uint64_t id = 0;
};
SourceParams paramsOf(const GpuSource &s) {
    return {s.firstBucket, s.availableFirstBucket, s.availableEndBucket, s.tfMs, s.clipPriceLo, s.clipPriceHi,
            s.ticks, uint32_t(s.bucketSlots.size()), s.pageShift(), s.wordsPerEntry(), s.wide, s.id};
}

struct HeatmapGpuBinner::SourceBuffers {
    std::shared_ptr<const GpuSource> source; // resident sources: only while uploading
    SourceParams params;
    std::unique_ptr<QRhiBuffer> bucketSlots, columnGroups, groups, runs, rowIndex;
    std::array<std::unique_ptr<QRhiBuffer>, kMaxEntryPages> pages;
    uint32_t pageCount = 0;
    struct Need { std::unique_ptr<QRhiBuffer> *slot; uint64_t bytes; QByteArray name; };
    std::vector<Need> needs; // capacity still to create, in order
    struct Part { std::unique_ptr<QRhiBuffer> *slot; const char *data; uint64_t bytes; };
    std::vector<Part> parts;
    size_t part = 0;
    uint64_t offset = 0;
    uint64_t bytes() const {
        uint64_t total = 0;
        for (auto *b : {bucketSlots.get(), columnGroups.get(), groups.get(), runs.get(), rowIndex.get()})
            if (b) total += b->size();
        for (const auto &p : pages) if (p) total += p->size();
        return total;
    }
};

struct HeatmapGpuBinner::SelfTestRun {
    std::shared_ptr<const PrecisionSelfTest> fixture; // built off the render thread
    std::unique_ptr<HeatmapGpuBinner> binner; // owns the readback
};

HeatmapGpuBinner::HeatmapGpuBinner(QRhi *rhi, uint64_t memoryCapBytes) : rhi_(rhi), memoryCapBytes_(memoryCapBytes) {
    prewarmPrecisionSelfTest(); // no-op after the first call; the build runs on a worker
    if (rhi_) rhi_->addCleanupCallback(this, [this](QRhi *) {
        RhiCleanupScope scope;
        releaseForDeadRhi();
    });
}
HeatmapGpuBinner::~HeatmapGpuBinner() {
    if (!rhi_) return; // the QRhi is gone and released everything through the callback
    if (rhiCleanupDepth > 0) removalsDuringCleanup.fetch_add(1); // a bug: see RhiCleanupScope
    rhi_->removeCleanupCallback(this);
    // QRhi keeps a raw pointer to the readback result until the frame that
    // recorded it completes and writes into it then (or in ~QRhi). Destroying
    // the binner first (window teardown right after the first frame) freed the
    // self-test's result and crashed in QRhiMetal::finishActiveReadbacks
    // (FM-099). finish() is valid inside and outside a frame (never inside a
    // pass; scene graph nodes are never destroyed there). The nested self-test
    // binner does the same for its own readback when selfTest_ is destroyed.
    if (readbackPending_) {
        rhi_->finish();
        drainedReadbacks.fetch_add(1);
    }
}

// The QRhi is being destroyed and still works: complete the readback into our
// result, then release every resource. Other binners (the self-test's nested
// one) get their own callback; never add or remove callbacks from here.
void HeatmapGpuBinner::releaseForDeadRhi() {
    if (readbackPending_) {
        rhi_->finish();
        drainedReadbacks.fetch_add(1);
    }
    readbackPending_ = false;
    active_.reset();
    spare_.reset();
    resident_.clear();
    pending_ = false;
    thresholds_.reset(); output_.reset(); computeParams_.reset(); dummy_.reset();
    computeBindings_.reset();
    for (auto &pipeline : compute_) pipeline.reset();
    computeLayout_.reset(); layoutUniform_.reset();
    binnedGrid_.reset();
    computeBoundTo_ = nullptr;
    rhi_ = nullptr; // inert from here on
}

uint64_t HeatmapGpuBinner::drainedReadbacksForTest() { return drainedReadbacks.load(); }
bool HeatmapGpuBinner::selfTestInFlightForTest() const { return selfTest_ != nullptr; }

void HeatmapGpuBinner::clearSelfTestCacheForTest() {
    std::scoped_lock lock(selfTestMutex);
    selfTestCache.clear();
}
QByteArray HeatmapGpuBinner::readBackData() const {
    return readback_ && !readbackPending_ ? readback_->data : QByteArray();
}
QString HeatmapGpuBinner::deviceKey() const {
    const auto info = rhi_->driverInfo();
    return QString::fromLatin1(rhi_->backendName()) + QLatin1Char('/') + QString::fromUtf8(info.deviceName) +
           QLatin1Char('/') + QString::number(info.deviceId, 16);
}
KernelVariant HeatmapGpuBinner::currentKernel() const { return resolved_.value_or(KernelVariant::Precise); }

std::shared_ptr<const GpuSource> HeatmapGpuBinner::activeSource() const {
    return active_ ? active_->source : nullptr;
}
std::shared_ptr<const GpuSource> HeatmapGpuBinner::pendingSource() const {
    return pending_ ? spare_->source : nullptr;
}
uint64_t HeatmapGpuBinner::sourceBytes() const { return active_ ? active_->bytes() : 0; }
uint64_t HeatmapGpuBinner::gpuBytes() const {
    uint64_t total = (active_ ? active_->bytes() : 0) + (spare_ ? spare_->bytes() : 0) + residentBytes();
    for (auto *b : {thresholds_.get(), output_.get(), computeParams_.get(), dummy_.get()})
        if (b) total += b->size();
    return total;
}

bool HeatmapGpuBinner::setSource(std::shared_ptr<const GpuSource> source, QString *error) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!source) return true;
    auto dropPending = [this] {
        pending_ = false;
        if (spare_) { spare_->source.reset(); spare_->needs.clear(); spare_->parts.clear(); }
    };
    if (active_ && active_->source == source) { // A -> B -> A: B must never activate
        if (pending_) dropPending();
        return true;
    }
    if (pending_ && spare_->source == source) return true;
    // A refused source is not retried every frame: never (limits, cap) or only
    // after its backoff (allocation failure). It was reported once already.
    if (refused_ && refused_->sourceId == source->id &&
        (!refused_->retryable || std::chrono::steady_clock::now() < refused_->retryAt)) {
        if (pending_) dropPending();
        return true;
    }
    if (refused_ && refused_->sourceId != source->id) refused_.reset();
    auto refuse = [&](const QString &message) {
        dropPending();
        refused_ = Refusal{source->id, false, {}, {}};
        return fail(error, message);
    };
    if (source->columnGroups.empty() || source->bucketSlots.empty())
        return refuse(QStringLiteral("empty heatmap GPU source"));
    if (source->entryPages() > kMaxEntryPages)
        return refuse(QStringLiteral("heatmap source needs more than %1 entry pages").arg(kMaxEntryPages));
    if (!spare_) spare_ = std::make_unique<SourceBuffers>();
    dropPending();
    auto &s = *spare_;
    const auto &src = *source;
    // name: the buffer's debug name (graphics debuggers, D3D12/Vulkan validation messages).
    struct Spec { std::unique_ptr<QRhiBuffer> *slot; const void *data; uint64_t bytes, limit; QByteArray name; };
    std::vector<Spec> specs{
        {&s.bucketSlots, src.bucketSlots.data(), src.bucketSlots.size() * 4ull, kMaxGpuBufferBytes, "heatmap.bucketSlots"},
        {&s.columnGroups, src.columnGroups.data(), src.columnGroups.size() * 4ull, kMaxGpuBufferBytes, "heatmap.columnGroups"},
        {&s.groups, src.groups.data(), src.groups.size() * sizeof(GroupMeta), kMaxGpuBufferBytes, "heatmap.groups"},
        {&s.runs, src.runs.data(), src.runs.size() * 8ull, kMaxGpuBufferBytes, "heatmap.runs"},
        {&s.rowIndex, src.rowIndex.data(), src.rowIndex.size() * 4ull, kMaxGpuBufferBytes, "heatmap.rowIndex"}};
    const uint64_t pageWords = src.entriesPerPage() * src.wordsPerEntry();
    const uint32_t pageCount = std::max<uint32_t>(src.entryPages(), 1);
    for (uint32_t p = 0; p < pageCount; ++p) {
        const uint64_t first = uint64_t(p) * pageWords;
        const uint64_t words = first < src.entries.size() ? std::min<uint64_t>(pageWords, src.entries.size() - first) : 0;
        specs.push_back({&s.pages[p], src.entries.data() + first, words * 4, pageWords * 4,
                         "heatmap.page" + QByteArray::number(p)});
    }
    auto neededOf = [](const Spec &spec) { return std::max<uint64_t>((spec.bytes + 15) / 16 * 16, 16); };
    auto grownOf = [&](const Spec &spec) { return std::min(spec.limit, neededOf(spec) + neededOf(spec) / 8); };
    // GPU memory cap over both buffer sets (active + spare after this upload).
    auto projected = [&] {
        uint64_t total = active_ ? active_->bytes() : 0;
        for (const auto &spec : specs) {
            const uint64_t have = *spec.slot ? (*spec.slot)->size() : 0;
            total += have >= neededOf(spec) ? have : grownOf(spec);
        }
        for (uint32_t p = pageCount; p < kMaxEntryPages; ++p) if (s.pages[p]) total += s.pages[p]->size();
        return total;
    };
    if (projected() > memoryCapBytes_) {
        for (uint32_t p = pageCount; p < kMaxEntryPages; ++p) s.pages[p].reset(); // spare capacity we can give back
        if (const uint64_t total = projected(); total > memoryCapBytes_)
            return refuse(QStringLiteral("heatmap source needs %1 MiB of GPU buffers with the active one; cap is %2 MiB")
                              .arg(total >> 20).arg(memoryCapBytes_ >> 20));
    }
    s.source = source;
    s.params = paramsOf(*source);
    s.part = 0;
    s.offset = 0;
    s.pageCount = pageCount;
    // Grow-only: reuse any buffer that is already large enough; plan the rest.
    for (const auto &spec : specs) {
        if (!*spec.slot || (*spec.slot)->size() < neededOf(spec)) s.needs.push_back({spec.slot, grownOf(spec), spec.name});
        if (spec.bytes) s.parts.push_back({spec.slot, static_cast<const char *>(spec.data), spec.bytes});
    }
    pending_ = true;
    return true;
}

bool HeatmapGpuBinner::uploadStep(QRhiCommandBuffer *cb, uint64_t budgetBytes, QString *error) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!pending_) return true;
    if (!cb || !budgetBytes) return fail(error, QStringLiteral("invalid upload step"));
    auto &s = *spare_;
    // Paged creation: at most one large buffer per step (small ones batch up).
    uint64_t created = 0;
    while (!s.needs.empty()) {
        const auto need = s.needs.front();
        if (created && created + need.bytes > kSmallAllocationBytes) break;
        auto buffer = failAllocationsForTest_ ? nullptr
            : makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, need.bytes, error, need.name);
        if (!buffer && failAllocationsForTest_ && error) *error = QStringLiteral("GPU buffer allocation failed (test)");
        if (!buffer) { // keep the active source; only the pending one is lost
            const uint64_t id = s.source->id;
            const auto backoff = refused_ && refused_->sourceId == id && refused_->retryable
                ? std::min(refused_->backoff * 2, kMaxRetryBackoff) : initialRetryBackoff_;
            refused_ = Refusal{id, true, std::chrono::steady_clock::now() + backoff, backoff};
            pending_ = false;
            s.source.reset(); s.needs.clear(); s.parts.clear();
            if (error) *error += QStringLiteral("; retrying this source in %1 ms").arg(backoff.count());
            return false;
        }
        *need.slot = std::move(buffer); // a replaced buffer is released by QRhi after in-flight frames
        created += need.bytes;
        s.needs.erase(s.needs.begin());
    }
    if (!s.needs.empty()) return true;
    auto *updates = rhi_->nextResourceUpdateBatch();
    uint64_t remaining = budgetBytes;
    while (s.part < s.parts.size() && remaining) {
        const auto &part = s.parts[s.part];
        const uint64_t chunk = std::min(part.bytes - s.offset, remaining);
        updates->uploadStaticBuffer(part.slot->get(), quint32(s.offset), quint32(chunk), part.data + s.offset);
        s.offset += chunk;
        remaining -= chunk;
        if (s.offset == part.bytes) { ++s.part; s.offset = 0; }
    }
    cb->resourceUpdate(updates);
    if (s.part >= s.parts.size()) {
        std::swap(active_, spare_);
        pending_ = false;
        if (refused_ && refused_->sourceId == active_->source->id) refused_.reset(); // a retry succeeded
        if (spare_) { // the retired set (none after the first upload) keeps its GPU capacity
            spare_->source.reset(); // but frees the retired source's CPU memory
            spare_->parts.clear();
        }
        computeBoundTo_ = nullptr; // bindings referenced the other buffer set
    }
    return true;
}

bool HeatmapGpuBinner::uploadAll(QRhiCommandBuffer *cb, QString *error) {
    while (pending_)
        if (!uploadStep(cb, 1ull << 40, error)) return false;
    return true;
}

namespace {
// The kernel's bindings, in one place for the real lists (active source, tiles)
// and the pipeline layout template (binner-owned placeholder buffers).
struct ComputeBuffers {
    QRhiBuffer *bucketSlots, *columnGroups, *groups, *runs, *thresholds, *output, *params, *rowIndex;
    std::array<QRhiBuffer *, kMaxEntryPages> pages;
};
QVarLengthArray<QRhiShaderResourceBinding, 20> computeBindingList(const ComputeBuffers &b) {
    const auto cs = QRhiShaderResourceBinding::ComputeStage;
    QVarLengthArray<QRhiShaderResourceBinding, 20> list{
        QRhiShaderResourceBinding::bufferLoad(0, cs, b.bucketSlots),
        QRhiShaderResourceBinding::bufferLoad(1, cs, b.columnGroups),
        QRhiShaderResourceBinding::bufferLoad(2, cs, b.groups),
        QRhiShaderResourceBinding::bufferLoad(3, cs, b.runs),
        QRhiShaderResourceBinding::bufferLoad(5, cs, b.thresholds),
        QRhiShaderResourceBinding::bufferLoadStore(6, cs, b.output), // the fill pass reads it
        QRhiShaderResourceBinding::uniformBuffer(7, cs, b.params),
        QRhiShaderResourceBinding::bufferLoad(8, cs, b.rowIndex)};
    for (uint32_t p = 0; p < kMaxEntryPages; ++p)
        list.append(QRhiShaderResourceBinding::bufferLoad(int(9 + p), cs, b.pages[p]));
    return list;
}
} // namespace

bool HeatmapGpuBinner::ensureLayouts(QString *error) {
    if (!dummy_) {
        dummy_ = makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, 16, error, "heatmap.dummy");
        if (!dummy_) return false;
    }
    if (!layoutUniform_) {
        layoutUniform_ = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ComputeParams), error);
        if (!layoutUniform_) return false;
    }
    QRhiBuffer *d = dummy_.get(), *u = layoutUniform_.get();
    auto make = [&](const auto &list, std::unique_ptr<QRhiShaderResourceBindings> &out) {
        if (out) return true;
        out.reset(rhi_->newShaderResourceBindings());
        out->setBindings(list.cbegin(), list.cend());
        if (out->create()) return true;
        out.reset();
        return fail(error, QStringLiteral("heatmap pipeline layout failed"));
    };
    ComputeBuffers placeholders{d, d, d, d, d, d, u, d, {}};
    placeholders.pages.fill(d);
    return make(computeBindingList(placeholders), computeLayout_);
}

std::unique_ptr<QRhiShaderResourceBindings> HeatmapGpuBinner::makeComputeBindings(const SourceBuffers &src,
                                                                                  QRhiBuffer *output, QRhiBuffer *params,
                                                                                  QString *error) {
    if (!ensureLayouts(error)) return {}; // also creates dummy_
    ComputeBuffers buffers{src.bucketSlots.get(), src.columnGroups.get(), src.groups.get(), src.runs.get(),
                           thresholds_.get(), output, params, src.rowIndex.get(), {}};
    for (uint32_t p = 0; p < kMaxEntryPages; ++p) {
        // Bind only this source's pages; spare capacity from older sources is never read.
        buffers.pages[p] = p < src.pageCount && src.pages[p] ? src.pages[p].get() : dummy_.get();
    }
    const auto list = computeBindingList(buffers);
    auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
    bindings->setBindings(list.cbegin(), list.cend());
    if (!bindings->create()) {
        fail(error, QStringLiteral("heatmap compute bindings failed"));
        return {};
    }
    return bindings;
}

bool HeatmapGpuBinner::rebuildComputeBindings(QString *error) {
    auto bindings = makeComputeBindings(*active_, output_.get(), computeParams_.get(), error);
    if (!bindings) return false;
    computeBindings_ = std::move(bindings); // layout-compatible with computeLayout_
    computeBoundTo_ = active_.get();
    return true;
}

bool HeatmapGpuBinner::ensurePipeline(KernelVariant variant, QString *error) {
    auto &pipeline = compute_[size_t(variant)];
    if (pipeline) return true;
    // D3D11 cannot run this kernel correctly (more UAVs than feature level 11_0
    // allows, no storage buffers in the fragment stage; at 11_1 it bins wrong
    // cells). Never draw wrong cells: refuse, like any other pipeline failure.
    if (rhi_->backend() == QRhi::D3D11) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            sLog_Error("heatmap gpu: Direct3D 11 is unsupported for the GPU heatmap; use Direct3D 12 "
                       "(QSG_RHI_BACKEND=d3d12, or SENTINEL_RHI_BACKEND=d3d12 for the lab and tests)");
        return fail(error, QStringLiteral("heatmap compute unsupported on D3D11"));
    }
    const QString path = variant == KernelVariant::Fast ? fastShaderPath_ : QString::fromLatin1(kPreciseShader);
    const QShader shader = loadShader(path);
    if (!shader.isValid()) return fail(error, QStringLiteral("compute shader missing: %1").arg(path));
    if (!ensureLayouts(error)) return false;
    pipeline.reset(rhi_->newComputePipeline());
    pipeline->setShaderStage({QRhiShaderStage::Compute, shader});
    // Every pipeline uses the binner-owned layout, never a source's or tile's
    // bindings (those are replaced or destroyed while the pipeline lives on).
    pipeline->setShaderResourceBindings(computeLayout_.get());
    if (!pipeline->create()) {
        pipeline.reset();
        return fail(error, QStringLiteral("heatmap compute pipeline failed"));
    }
    return true;
}

void HeatmapGpuBinner::driveSelfTest(QRhiCommandBuffer *cb) {
    if (resolved_ || !rhi_) return;
    if (!testShader_) {
        std::scoped_lock lock(selfTestMutex);
        if (const auto it = selfTestCache.find(deviceKey()); it != selfTestCache.end()) {
            resolved_ = it->second;
            return;
        }
    }
    if (!selfTest_) {
        // Only once there is something to draw, and only with the fixture that
        // a worker built: prepare() records the dispatch and readback, nothing more.
        if (!active_ && resident_.empty()) return;
        auto fixture = precisionSelfTestIfReady();
        if (!fixture) return;
        // Run the fast kernel on the fixture in its own binner; the readback
        // lands when this frame completes. Meanwhile bins use the precise kernel.
        selfTest_ = std::make_unique<SelfTestRun>();
        auto &run = *selfTest_;
        run.fixture = std::move(fixture);
        run.binner = std::make_unique<HeatmapGpuBinner>(rhi_);
        run.binner->fastShaderPath_ = fastShaderPath_;
        run.binner->forceKernel(KernelVariant::Fast);
        QString error;
        auto source = std::make_shared<const GpuSource>(run.fixture->source);
        if (!run.binner->setSource(source, &error) || !run.binner->uploadAll(cb, &error) ||
            !run.binner->bin(cb, run.fixture->grid, {}, &error) || !run.binner->readBack(cb, &error)) {
            sLog_Warning("heatmap gpu: precision self-test could not run (" << error
                         << "); using the precise kernel");
            resolved_ = KernelVariant::Precise;
            selfTest_.reset();
        }
        return;
    }
    auto &run = *selfTest_;
    if (run.binner->readBackPending()) return; // still in flight
    const QByteArray data = run.binner->readBackData();
    std::vector<uint32_t> cells(run.fixture->expected.size());
    if (data.size() == qsizetype(cells.size() * 4)) std::memcpy(cells.data(), data.constData(), cells.size() * 4);
    else std::fill(cells.begin(), cells.end(), 0xffffffffu); // a failed readback fails the test
    const size_t mismatches = countSelfTestMismatches(*run.fixture, cells);
    resolved_ = mismatches ? KernelVariant::Precise : KernelVariant::Fast;
    if (mismatches)
        sLog_Warning("heatmap gpu: fast kernel failed the precision self-test on " << deviceKey() << " ("
                     << mismatches << "/" << cells.size() << " cells differ from binColumn); using the precise kernel");
    else
        sLog_App("heatmap gpu: fast kernel passed the precision self-test on " << deviceKey() << " ("
                    << cells.size() << " cells)");
    if (!testShader_) {
        std::scoped_lock lock(selfTestMutex);
        selfTestCache[deviceKey()] = *resolved_;
    }
    selfTest_.reset(); // its frame has completed, so its resources are idle
}

bool HeatmapGpuBinner::binnedMatches(uint64_t sourceId, const recording::SizeScale &outputScale) const {
    return binnedGrid_ && binnedSourceId_ == sourceId && sameScale(binnedScale_, outputScale) &&
           binnedKernel_ == currentKernel();
}

bool HeatmapGpuBinner::bin(QRhiCommandBuffer *cb, const BinGrid &grid,
                           const recording::SizeScale &outputScale, QString *error) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!active_) return fail(error, QStringLiteral("no uploaded heatmap source"));
    const auto &source = active_->params;
    if (!cb || grid.tfMs != source.tfMs || !grid.columns || !grid.rows ||
        grid.columns > kMaxGridColumns || grid.rows > kMaxGridRows || !(grid.displayTick > 0))
        return fail(error, QStringLiteral("invalid heatmap output grid"));
    driveSelfTest(cb);
    if (!computeParams_) {
        computeParams_ = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ComputeParams), error);
        if (!computeParams_) return false;
    }
    auto *updates = rhi_->nextResourceUpdateBatch();
    auto bail = [&] {
        updates->release();
        return false;
    };
    if (!ensureThresholds(updates, outputScale, error)) return bail();
    const uint64_t cellBytes = uint64_t(grid.columns) * grid.rows * 4;
    if (!output_ || output_->size() < cellBytes) {
        output_ = makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, cellBytes + cellBytes / 4, error,
                             "heatmap.output");
        if (!output_) return bail();
        computeBoundTo_ = nullptr;
    }
    if (computeBoundTo_ != active_.get() && !rebuildComputeBindings(error)) return bail();
    const KernelVariant kernel = currentKernel();
    if (!ensurePipeline(kernel, error)) return bail();
    ComputeParams params{};
    if (!fillComputeParams(&source, grid, outputScale, false, &params, error)) return bail();
    updates->updateDynamicBuffer(computeParams_.get(), 0, sizeof(params), &params);
    cb->beginComputePass(updates);
    cb->setComputePipeline(compute_[size_t(kernel)].get());
    cb->setShaderResources(computeBindings_.get());
    cb->dispatch(int((grid.columns + 15) / 16), int((grid.rows + 3) / 4), 1); // heatmap_bin.comp local size
    cb->endComputePass();
    binnedGrid_ = grid;
    binnedSourceId_ = source.id;
    binnedScale_ = outputScale;
    binnedKernel_ = kernel;
    return true;
}

bool HeatmapGpuBinner::ensureThresholds(QRhiResourceUpdateBatch *updates, const recording::SizeScale &outputScale,
                                        QString *error) {
    if (thresholds_ && sameScale(thresholdScale_, outputScale)) return true;
    const auto &table = cachedEncodeThresholds(outputScale);
    if (!thresholds_) {
        thresholds_ = makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer,
                                 table.size() * sizeof(FloatFloat), error, "heatmap.thresholds");
        if (!thresholds_) return false;
        computeBoundTo_ = nullptr;
    }
    updates->uploadStaticBuffer(thresholds_.get(), 0, quint32(table.size() * sizeof(FloatFloat)), table.data());
    thresholdScale_ = outputScale;
    return true;
}

bool HeatmapGpuBinner::fillComputeParams(const void *sourceParams, const BinGrid &grid,
                                         const recording::SizeScale &outputScale, bool fill, void *out,
                                         QString *error) const {
    const auto &source = *static_cast<const SourceParams *>(sourceParams);
    const auto factors = tickFactors(source.ticks, grid.displayTick);
    uint32_t maxFactor = 0;
    for (const auto f : factors) maxFactor = std::max(maxFactor, f);
    const double topRow = double(std::max(std::abs(grid.firstBin), std::abs(grid.firstBin + grid.rows))) * maxFactor;
    if (topRow >= double(std::numeric_limits<int32_t>::max()))
        return fail(error, QStringLiteral("heatmap grid native rows exceed int32"));
    constexpr double big = double(1 << 30);
    auto &params = *static_cast<ComputeParams *>(out);
    params.dims = {grid.columns, grid.rows, source.slotCount, (source.wide ? 1u : 0u) | (fill ? 2u : 0u)};
    params.time = {clampToInt32(double(grid.firstBucket - source.firstBucket), -big, big),
                   clampToInt32(double(source.availableFirstBucket - grid.firstBucket), 0, grid.columns),
                   clampToInt32(double(source.availableEndBucket - grid.firstBucket), 0, grid.columns),
                   int32_t(thresholds_->size() / sizeof(FloatFloat) >= recording::kMaxCode - 1 ?
                           recording::kMaxCode - 1 : 0)};
    params.price = {int32_t(grid.firstBin),
                    clampToInt32(std::ceil(source.clipPriceLo / grid.displayTick - 1e-9), -big, big),
                    clampToInt32(std::floor(source.clipPriceHi / grid.displayTick + 1e-9), -big, big), 0};
    params.factors = factors;
    params.codeScale = {float(outputScale.floor), float(outputScale.codesPerOctave), 0, 0};
    params.paging = {source.pageShift, source.wordsPerEntry, 0, 0};
    return true;
}

HeatmapGpuBinner::PassCache::PassCache() = default;
HeatmapGpuBinner::PassCache::~PassCache() {
    // A frame in flight may still reference them.
    if (params) params.release()->deleteLater();
    if (bindings) bindings.release()->deleteLater();
}
HeatmapGpuBinner::PassCache::PassCache(PassCache &&) noexcept = default;
HeatmapGpuBinner::PassCache &HeatmapGpuBinner::PassCache::operator=(PassCache &&other) noexcept {
    if (this == &other) return *this;
    if (params) params.release()->deleteLater();
    if (bindings) bindings.release()->deleteLater();
    params = std::move(other.params);
    bindings = std::move(other.bindings);
    boundTo = other.boundTo;
    created = other.created;
    return *this;
}

bool HeatmapGpuBinner::binSourceInto(const SourceBuffers &src, QRhiCommandBuffer *cb, const BinGrid &grid,
                                     const recording::SizeScale &outputScale, QRhiBuffer *target, bool fill,
                                     QString *error, PassCache *cache) {
    const auto &source = src.params;
    if (!cb || !target || grid.tfMs != source.tfMs || !grid.columns || !grid.rows ||
        grid.columns > kMaxGridColumns || grid.rows > kMaxGridRows || !(grid.displayTick > 0) ||
        target->size() < uint64_t(grid.columns) * grid.rows * 4)
        return fail(error, QStringLiteral("invalid heatmap tile grid or target"));
    driveSelfTest(cb);
    auto *updates = rhi_->nextResourceUpdateBatch();
    // A Dynamic uniform buffer is written when its batch is committed, so several
    // bins recorded in one frame each need their own parameter buffer (a cache
    // serves one pass per frame).
    std::unique_ptr<QRhiBuffer> transientParams;
    QRhiBuffer *params = nullptr;
    if (cache) {
        if (!cache->params) {
            cache->params = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ComputeParams), error);
            cache->created += cache->params ? 1 : 0;
        }
        params = cache->params.get();
    } else {
        transientParams = makeBuffer(rhi_, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(ComputeParams), error);
        params = transientParams.get();
    }
    if (!params || !ensureThresholds(updates, outputScale, error) || !ensureLayouts(error)) {
        updates->release();
        return false;
    }
    std::unique_ptr<QRhiShaderResourceBindings> transientBindings;
    QRhiShaderResourceBindings *bindings = nullptr;
    if (cache) {
        // The buffers the bindings reference: rebuild only when one changed.
        std::array<const QRhiBuffer *, 8 + kMaxEntryPages> bound{
            src.bucketSlots.get(), src.columnGroups.get(), src.groups.get(), src.runs.get(), thresholds_.get(),
            target, params, src.rowIndex.get()};
        for (uint32_t p = 0; p < kMaxEntryPages; ++p)
            bound[8 + p] = p < src.pageCount && src.pages[p] ? src.pages[p].get() : dummy_.get();
        if (!cache->bindings || bound != cache->boundTo) {
            auto fresh = makeComputeBindings(src, target, params, error);
            if (!fresh) { updates->release(); return false; }
            if (cache->bindings) cache->bindings.release()->deleteLater();
            cache->bindings = std::move(fresh);
            cache->boundTo = bound;
        }
        bindings = cache->bindings.get();
    } else {
        transientBindings = makeComputeBindings(src, target, params, error);
        bindings = transientBindings.get();
    }
    const KernelVariant kernel = currentKernel();
    if (!bindings || !ensurePipeline(kernel, error)) { updates->release(); return false; }
    ComputeParams values{};
    if (!fillComputeParams(&source, grid, outputScale, fill, &values, error)) { updates->release(); return false; }
    updates->updateDynamicBuffer(params, 0, sizeof(values), &values);
    cb->beginComputePass(updates);
    cb->setComputePipeline(compute_[size_t(kernel)].get());
    cb->setShaderResources(bindings);
    cb->dispatch(int((grid.columns + 15) / 16), int((grid.rows + 3) / 4), 1); // heatmap_bin.comp local size
    cb->endComputePass();
    // Released by QRhi once the frames recording them have completed.
    if (transientParams) transientParams.release()->deleteLater();
    if (transientBindings) transientBindings.release()->deleteLater();
    return true;
}

bool HeatmapGpuBinner::uploadResident(const std::shared_ptr<const GpuSource> &source, QRhiCommandBuffer *cb,
                                      uint64_t &budget, bool *complete, QString *error) {
    *complete = false;
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!source || !cb) return fail(error, QStringLiteral("invalid resident source"));
    if (source->entryPages() > kMaxEntryPages)
        return fail(error, QStringLiteral("heatmap source needs more than %1 entry pages").arg(kMaxEntryPages));
    auto &slot = resident_[source->id];
    if (!slot) {
        // Exact-size buffers, created at once (a tile source is a few MiB).
        auto s = std::make_unique<SourceBuffers>();
        s->source = source;
        s->params = paramsOf(*source);
        const auto &src = *source;
        struct Spec { std::unique_ptr<QRhiBuffer> *slot; const void *data; uint64_t bytes; };
        std::vector<Spec> specs{{&s->bucketSlots, src.bucketSlots.data(), src.bucketSlots.size() * 4ull},
                                {&s->columnGroups, src.columnGroups.data(), src.columnGroups.size() * 4ull},
                                {&s->groups, src.groups.data(), src.groups.size() * sizeof(GroupMeta)},
                                {&s->runs, src.runs.data(), src.runs.size() * 8ull},
                                {&s->rowIndex, src.rowIndex.data(), src.rowIndex.size() * 4ull}};
        const uint64_t pageWords = src.entriesPerPage() * src.wordsPerEntry();
        s->pageCount = std::max<uint32_t>(src.entryPages(), 1);
        for (uint32_t p = 0; p < s->pageCount; ++p) {
            const uint64_t first = uint64_t(p) * pageWords;
            const uint64_t words = first < src.entries.size() ? std::min<uint64_t>(pageWords, src.entries.size() - first) : 0;
            specs.push_back({&s->pages[p], src.entries.data() + first, words * 4});
        }
        for (const auto &spec : specs) {
            *spec.slot = failAllocationsForTest_ ? nullptr
                : makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, spec.bytes, error);
            if (!*spec.slot) {
                resident_.erase(source->id);
                return fail(error, QStringLiteral("resident source allocation failed"));
            }
            if (spec.bytes) s->parts.push_back({spec.slot, static_cast<const char *>(spec.data), spec.bytes});
        }
        slot = std::move(s);
    }
    auto &s = *slot;
    if (s.part < s.parts.size() && budget) {
        auto *updates = rhi_->nextResourceUpdateBatch();
        while (s.part < s.parts.size() && budget) {
            const auto &part = s.parts[s.part];
            const uint64_t chunk = std::min(part.bytes - s.offset, budget);
            updates->uploadStaticBuffer(part.slot->get(), quint32(s.offset), quint32(chunk), part.data + s.offset);
            s.offset += chunk;
            budget -= chunk;
            if (s.offset == part.bytes) { ++s.part; s.offset = 0; }
        }
        cb->resourceUpdate(updates);
    }
    *complete = s.part >= s.parts.size();
    if (*complete) { // the CPU image may go; the GPU copy stays
        s.parts.clear();
        s.source.reset();
    }
    return true;
}

bool HeatmapGpuBinner::isResident(uint64_t sourceId) const {
    const auto it = resident_.find(sourceId);
    return it != resident_.end() && it->second->part >= it->second->parts.size();
}

bool HeatmapGpuBinner::binResidentInto(uint64_t sourceId, QRhiCommandBuffer *cb, const BinGrid &grid,
                                       const recording::SizeScale &outputScale, QRhiBuffer *target, QString *error,
                                       bool fill, PassCache *cache) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!isResident(sourceId)) return fail(error, QStringLiteral("heatmap source is not resident"));
    return binSourceInto(*resident_.at(sourceId), cb, grid, outputScale, target, fill, error, cache);
}

bool HeatmapGpuBinner::beginRefill(uint64_t residentId, std::shared_ptr<const GpuSource> source, bool shrink,
                                   uint64_t *created, QString *error) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!source) return fail(error, QStringLiteral("invalid live source"));
    if (source->entryPages() > kMaxEntryPages)
        return fail(error, QStringLiteral("heatmap source needs more than %1 entry pages").arg(kMaxEntryPages));
    auto &slot = resident_[residentId];
    if (!slot) slot = std::make_unique<SourceBuffers>();
    auto &s = *slot;
    const auto &src = *source;
    struct Spec { std::unique_ptr<QRhiBuffer> *slot; const void *data; uint64_t bytes; };
    std::vector<Spec> specs{{&s.bucketSlots, src.bucketSlots.data(), src.bucketSlots.size() * 4ull},
                            {&s.columnGroups, src.columnGroups.data(), src.columnGroups.size() * 4ull},
                            {&s.groups, src.groups.data(), src.groups.size() * sizeof(GroupMeta)},
                            {&s.runs, src.runs.data(), src.runs.size() * 8ull},
                            {&s.rowIndex, src.rowIndex.data(), src.rowIndex.size() * 4ull}};
    const uint64_t pageWords = src.entriesPerPage() * src.wordsPerEntry();
    const uint32_t pageCount = std::max<uint32_t>(src.entryPages(), 1);
    for (uint32_t p = 0; p < pageCount; ++p) {
        const uint64_t first = uint64_t(p) * pageWords;
        const uint64_t words = first < src.entries.size() ? std::min<uint64_t>(pageWords, src.entries.size() - first) : 0;
        specs.push_back({&s.pages[p], src.entries.data() + first, words * 4});
    }
    s.parts.clear();
    s.part = s.offset = 0;
    s.source.reset();
    for (uint32_t p = pageCount; p < kMaxEntryPages; ++p) // capacity no image of this size needs
        if (s.pages[p]) s.pages[p].release()->deleteLater();
    for (const auto &spec : specs) {
        const uint64_t need = std::max<uint64_t>((spec.bytes + 15) / 16 * 16, 16);
        // Headroom: the forming bucket grows during the minute.
        const uint64_t grown = std::min<uint64_t>(std::max(need + need / 2, uint64_t(4096)),
                                                  std::max(need, kMaxGpuBufferBytes));
        const uint64_t have = *spec.slot ? (*spec.slot)->size() : 0;
        const bool replace = have < need || (shrink && have > 2 * grown);
        if (replace) {
            auto buffer = failAllocationsForTest_ ? nullptr
                : makeBuffer(rhi_, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, grown, error, "heatmap.live");
            if (!buffer) {
                if (failAllocationsForTest_ && error) *error = QStringLiteral("GPU buffer allocation failed (test)");
                resident_.erase(residentId); // never half-filled: the caller starts again
                return false;
            }
            if (*spec.slot) spec.slot->release()->deleteLater(); // a frame in flight may still read it
            *spec.slot = std::move(buffer);
            if (created) ++*created;
        }
        if (spec.bytes) s.parts.push_back({spec.slot, static_cast<const char *>(spec.data), spec.bytes});
    }
    s.source = std::move(source); // keeps the image alive while it pages in
    s.params = paramsOf(*s.source);
    s.pageCount = pageCount;
    if (s.parts.empty()) s.source.reset();
    return true;
}

bool HeatmapGpuBinner::refillStep(uint64_t residentId, QRhiCommandBuffer *cb, uint64_t &budget, bool *complete,
                                  QString *error) {
    *complete = false;
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    const auto it = resident_.find(residentId);
    if (it == resident_.end() || !cb) return fail(error, QStringLiteral("no live refill in progress"));
    auto &s = *it->second;
    if (s.part < s.parts.size() && budget) {
        auto *updates = rhi_->nextResourceUpdateBatch();
        while (s.part < s.parts.size() && budget) {
            const auto &part = s.parts[s.part];
            const uint64_t chunk = std::min(part.bytes - s.offset, budget);
            updates->uploadStaticBuffer(part.slot->get(), quint32(s.offset), quint32(chunk), part.data + s.offset);
            s.offset += chunk;
            budget -= chunk;
            if (s.offset == part.bytes) { ++s.part; s.offset = 0; }
        }
        cb->resourceUpdate(updates);
    }
    *complete = s.part >= s.parts.size();
    if (*complete) {
        s.parts.clear();
        s.part = 0;
        s.source.reset();
    }
    return true;
}

void HeatmapGpuBinner::releaseResident(uint64_t sourceId) {
    const auto it = resident_.find(sourceId);
    if (it == resident_.end()) return;
    auto &s = *it->second;
    for (auto *slot : {&s.bucketSlots, &s.columnGroups, &s.groups, &s.runs, &s.rowIndex})
        if (*slot) slot->release()->deleteLater();
    for (auto &page : s.pages)
        if (page) page.release()->deleteLater();
    resident_.erase(it);
}
uint64_t HeatmapGpuBinner::residentBytes(uint64_t sourceId) const {
    const auto it = resident_.find(sourceId);
    return it == resident_.end() ? 0 : it->second->bytes();
}

uint64_t HeatmapGpuBinner::residentBytes() const {
    uint64_t total = 0;
    for (const auto &[id, s] : resident_) total += s->bytes();
    return total;
}

bool HeatmapGpuBinner::readBack(QRhiCommandBuffer *cb, QString *error) {
    if (!rhi_) return fail(error, QStringLiteral("QRhi destroyed"));
    if (!binnedGrid_ || !output_ || !cb) return fail(error, QStringLiteral("nothing binned to read back"));
    if (readbackPending_) return fail(error, QStringLiteral("a readback is already in flight"));
    if (!readback_) readback_ = std::make_unique<QRhiReadbackResult>();
    readback_->data.clear();
    // Runs on the thread that completes the frame (the render thread) while the
    // result is alive: the destructor and the QRhi cleanup callback finish() first.
    readback_->completed = [this] { readbackPending_ = false; };
    readbackPending_ = true;
    const uint64_t bytes = uint64_t(binnedGrid_->columns) * binnedGrid_->rows * 4;
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->readBackBuffer(output_.get(), 0, quint32(bytes), readback_.get());
    cb->resourceUpdate(updates);
    return true;
}

} // namespace heatmap::gpu
