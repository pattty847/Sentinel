#include "HeatmapCellQuery.hpp"
#include "heatmap/BinCell.hpp"
#include "heatmap/DrawPieces.hpp"
#include "SentinelLogging.hpp"
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace heatmap {
namespace {
void formatAmount(std::array<char, 48>& out, double value, bool usd, const std::string& asset) {
    if (!(value > 0) || !std::isfinite(value)) return;
    constexpr const char* suffix[] = {"", "k", "M", "B", "T"};
    int unit = 0;
    while (value >= 999.5 && unit < 4) { value /= 1000; ++unit; }
    const int decimals = std::clamp(2 - int(std::floor(std::log10(value))), 0, 15);
    char* at = out.data();
    if (usd) *at++ = '$';
    const auto result = std::to_chars(at, out.data() + 28, value, std::chars_format::fixed, decimals);
    if (result.ec != std::errc{}) { out[0] = 0; return; }
    char* end = result.ptr;
    if (decimals) {
        while (end > at && end[-1] == '0') --end;
        if (end > at && end[-1] == '.') --end;
    }
    if (unit) *end++ = *suffix[unit];
    if (!usd && !asset.empty()) {
        *end++ = ' ';
        const auto n = std::min<size_t>(asset.size(), size_t(out.data() + out.size() - 1 - end));
        std::memcpy(end, asset.data(), n);
        end += n;
    }
    *end = 0;
}
}

std::shared_ptr<const SparseColumns> LabelWindowBuilder::composeWindow(const SpanSourceKey& key,
    int64_t fromMs, int64_t toMs, ComposeOptions::PriceClip price, ChunkStore& store, LabelCells& result) {
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if (it->key == key && it->fromMs <= fromMs && it->toMs >= toMs &&
            it->price.lo <= price.lo && it->price.end >= price.end) {
            auto out = it->columns;
            windows_.splice(windows_.begin(), windows_, it);
            ++result.cacheHits;
            return out;
        }
    }
    std::vector<std::shared_ptr<const StoredChunk>> chunks;
    std::vector<const SparseColumns*> inputs;
    bool missing = false;
    for (const auto& generation : key.generations) {
        const ChunkKey chunkKey{generation.symbol, generation.source, generation.levelMs, generation.startMs};
        const auto spanMs = generation.levelMs == kHourMs ? kDayMs : kHourMs;
        if (generation.startMs >= toMs || generation.startMs + spanMs <= fromMs) continue;
        auto chunk = store.peek(chunkKey);
        if (!chunk || chunk->generation != generation.generation) {
            result.missing.push_back(generation);
            missing = true;
        } else {
            inputs.push_back(chunk->columns.get());
            chunks.push_back(std::move(chunk));
        }
    }
    if (missing) return {};
    if (inputs.empty()) return std::make_shared<const SparseColumns>(SparseColumns{
        key.span.symbol, "", key.span.tfMs, fromMs, toMs, {}, {}});
    ComposeOptions options;
    options.startMs = fromMs;
    options.endMs = toMs;
    options.price = price;
    options.trustedInputs = true;
    auto columns = std::make_shared<const SparseColumns>(compose(inputs, key.span.tfMs, options));
    ++result.composedWindows;
    const auto bytes = sparseBytes(*columns);
    if (bytes <= maxBytes_) {
        while (bytes_ + bytes > maxBytes_ && !windows_.empty()) {
            bytes_ -= windows_.back().bytes;
            windows_.pop_back();
        }
        windows_.push_front({key, fromMs, toMs, price, columns, bytes});
        bytes_ += bytes;
    }
    return columns;
}

std::shared_ptr<const LabelCells> LabelWindowBuilder::build(const LabelRequest& request, const SpanSet& spans,
                                                          const LiveSnapshot* live, ChunkStore& store) {
    const double tick = fromUnits(request.tickUnits, request.priceScale);
    if (request.tfMs < kMinuteMs || request.tfMs > kDayMs || request.tfMs % kMinuteMs ||
        request.firstBucket < 0 || request.firstBucket > INT64_MAX / request.tfMs - request.columns ||
        !request.rows || request.rows > 16384 || !request.columns || uint64_t(request.rows) * request.columns > 16000 ||
        request.firstBin < 0 || !std::isfinite(tick) || tick <= 0 ||
        double(request.firstBin) + request.rows > 0x1p52 || spans.tfMs != request.tfMs ||
        (request.spanVersion && request.spanVersion != spans.version) ||
        (request.liveVersion && (!live || request.liveVersion != live->version)))
        throw std::invalid_argument("invalid label window or picture version");
    auto out = std::make_shared<LabelCells>();
    out->key = request;
    out->grid = {request.tfMs, tiles::tileOfBucket(request.firstBucket), request.firstBucket, request.columns,
                 request.tickUnits, tick, request.priceScale, request.firstBin, request.rows};
    out->cells.resize(size_t(request.columns) * request.rows);
    out->columnStates.resize(request.columns, BucketState::NotLoaded);
    out->formingColumns.resize(request.columns, false);
    const auto fromMs = request.firstBucket * request.tfMs;
    const ComposeOptions::PriceClip price{request.firstBin * tick, (request.firstBin + request.rows) * tick};
    for (uint32_t x = 0; x < request.columns; ++x) {
        const auto time = fromMs + x * request.tfMs;
        const bool available = time + request.tfMs > spans.availableStartMs && time < spans.availableEndMs;
        if (available) for (uint32_t y = 0; y < request.rows; ++y)
            out->cells[size_t(y) * request.columns + x].word = tiles::cellWord(tiles::kCellLoading);
    }
    std::vector<DrawSpan> drawn;
    for (size_t i = 0; i < spans.spans.size(); ++i) {
        const auto& span = spans.spans[i];
        if (span.id.tfMs != request.tfMs) continue;
        int64_t completeEnd = INT64_MAX;
        for (const auto& s : span.sources) if (s.build) completeEnd = std::min(completeEnd, s.build->completeEndMs);
        if (completeEnd != INT64_MAX) drawn.push_back({i, span.id.tile, span.id.tfMs, completeEnd});
    }
    std::vector<DrawLive> liveDraws;
    if (live && live->tfMs == request.tfMs && live->symbol == spans.symbol && !live->sources.empty()) {
        int64_t start = INT64_MAX, end = 0;
        for (const auto& s : live->sources) if (s.columns) {
            start = std::min(start, s.startMs);
            end = std::max(end, recording::floorDiv(s.openEndMs + request.tfMs - 1, request.tfMs) * request.tfMs);
        }
        if (start < end) liveDraws.push_back({0, request.tfMs, start, end});
    }
    const auto pieces = drawPieces(drawn, liveDraws, request.tfMs);
    std::vector<bool> reused(request.columns, false);
    auto sameHistory = [&] {
        if (!previous_ || previous_->key.liveVersion == request.liveVersion || previous_->key.spanVersion != request.spanVersion)
            return false;
        auto old = previous_->key, now = request;
        old.serial = now.serial = 0; old.liveVersion = now.liveVersion = 0;
        return old == now;
    };
    for (const auto& piece : pieces) {
        const auto first = std::max(request.firstBucket, recording::floorDiv(piece.loMs + request.tfMs - 1, request.tfMs));
        const auto end = std::min(request.firstBucket + request.columns,
                                 recording::floorDiv(piece.hiMs + request.tfMs - 1, request.tfMs));
        if (end <= first) continue;
        const auto x0 = uint32_t(first - request.firstBucket), x1 = uint32_t(end - request.firstBucket);
        if (!piece.live && sameHistory() && std::find(previousPieces_.begin(), previousPieces_.end(), piece) != previousPieces_.end()) {
            for (uint32_t x = x0; x < x1; ++x) {
                out->columnStates[x] = previous_->columnStates[x]; reused[x] = true;
                for (uint32_t y = 0; y < request.rows; ++y)
                    out->cells[size_t(y) * request.columns + x] = previous_->cells[size_t(y) * request.columns + x];
            }
            out->reusedHistoryColumns += x1 - x0;
            continue;
        }
        bool fill = false;
        auto hasVeil = [&] {
            for (uint32_t y = 0; y < request.rows; ++y) for (uint32_t x = x0; x < x1; ++x)
                if (tiles::cellState(out->cells[size_t(y) * request.columns + x].word) == tiles::kCellVeil) return true;
            return false;
        };
        // The same column sweep as buildCells, retaining binColumn's exact
        // double alongside its word; avoids binning twice to recover the value.
        auto apply = [&](const SparseColumns* columns, int64_t availableStart, int64_t availableEnd) {
            for (uint32_t x = x0; x < x1; ++x) {
                const auto bucket = (request.firstBucket + x) * request.tfMs;
                uint32_t word = tiles::cellWord(tiles::kCellNoData);
                std::vector<BinCell> cells;
                if (bucket + request.tfMs > availableStart && bucket < availableEnd) {
                    const auto state = columns ? bucketState(*columns, bucket) : BucketState::NotLoaded;
                    if (!fill || state == BucketState::Present) out->columnStates[x] = state;
                    if (piece.live && bucket == liveDraws.front().endMs - request.tfMs)
                        out->formingColumns[x] = true;
                    word = tiles::cellWord(state == BucketState::NotLoaded ? tiles::kCellLoading : tiles::kCellVeil);
                    if (state == BucketState::Present) {
                        const auto it = std::lower_bound(columns->columns.begin(), columns->columns.end(), bucket,
                            [](const auto& c, int64_t t) { return c.bucketStartMs < t; });
                        cells = binColumn(*it, price.lo, price.end, tick);
                    }
                }
                for (uint32_t y = 0; y < request.rows; ++y) {
                    auto& cell = out->cells[size_t(y) * request.columns + x];
                    const bool valid = !cells.empty() && cells[y].valid;
                    if (fill && (tiles::cellState(cell.word) != tiles::kCellVeil || !valid)) continue;
                    cell.word = valid ? tiles::cellWord(tiles::kCellValid, cells[y].code, cells[y].dominantAsk) : word;
                    cell.value = valid ? (cells[y].dominantAsk ? cells[y].ask : cells[y].bid) : 0;
                }
            }
            fill = true;
        };
        if (piece.live) {
            for (const auto& s : live->sources) {
                if (fill && !hasVeil()) break;
                apply(s.columns.get(), s.startMs, s.openEndMs);
            }
        } else {
            for (const auto& s : spans.spans[piece.token].sources) {
                if (!s.build) continue;
                if (fill && !hasVeil()) break;
                auto columns = composeWindow(s.build->key, first * request.tfMs, end * request.tfMs, price, store, *out);
                apply(columns.get(), s.build->key.availableStartMs, s.build->key.availableEndMs);
            }
        }
    }
    if (request.formatText) for (uint32_t y = 0; y < request.rows; ++y) {
        const double mid = (request.firstBin + request.rows - y - 0.5) * tick;
        for (uint32_t x = 0; x < request.columns; ++x) {
            if (reused[x]) continue;
            auto& cell = out->cells[size_t(y) * request.columns + x];
            if (tiles::cellState(cell.word) != tiles::kCellValid || !(cell.word & 0x7fff)) continue;
            formatAmount(cell.usd, cell.value * mid, true, request.asset);
            formatAmount(cell.asset, cell.value, false, request.asset);
        }
    }
    if (out->missing.empty()) { previous_ = out; previousPieces_ = pieces; }
    return out;
}

namespace {
struct WallWindow {
    int64_t from = 0, to = 0, firstBucket = 0, endBucket = 0, firstBin = 0, endBin = 0, units = 0;
    double lo = 0, hi = 0, tick = 0;
};
std::optional<WallWindow> wallWindow(const WallScanRequest& r) {
    const auto& q = r.query;
    if (r.tfMs < kMinuteMs || r.tfMs > kDayMs || r.tfMs % kMinuteMs ||
        q.limit < 1 || q.limit > 100 || !std::isfinite(q.minQty) || q.minQty < 0 ||
        !std::isfinite(r.priceScale) || r.priceScale <= 0) return {};
    WallWindow w;
    const double from = q.startMs ? double(*q.startMs) : r.timeLoMs;
    const double to = q.endMs ? double(*q.endMs) : r.timeHiMs;
    const double height = r.priceHi - r.priceLo;
    w.lo = q.priceMin.value_or(std::max(0.0, r.priceLo - height));
    w.hi = q.priceMax.value_or(r.priceHi + height);
    w.tick = q.tick.value_or(fromUnits(r.drawnTickUnits, r.priceScale));
    if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to <= from || to >= 0x1p52 ||
        !std::isfinite(w.lo) || !std::isfinite(w.hi) || w.lo < 0 || w.hi <= w.lo ||
        !std::isfinite(w.tick) || w.tick <= 0 || w.tick * r.priceScale >= 0x1p52) return {};
    w.units = toUnits(w.tick, r.priceScale);
    if (w.units <= 0 || std::abs(fromUnits(w.units, r.priceScale) - w.tick) > w.tick * 1e-9 ||
        w.hi / w.tick >= 0x1p52) return {};
    w.from = int64_t(std::ceil(from)); w.to = int64_t(std::ceil(to));
    w.firstBucket = recording::floorDiv(w.from + r.tfMs - 1, r.tfMs);
    w.endBucket = recording::floorDiv(w.to + r.tfMs - 1, r.tfMs);
    w.firstBin = int64_t(std::floor(w.lo / w.tick)); w.endBin = int64_t(std::ceil(w.hi / w.tick));
    if (w.endBin <= w.firstBin || w.endBucket < w.firstBucket ||
        uint64_t(w.endBin - w.firstBin) > 16'000'000 ||
        uint64_t(w.endBucket - w.firstBucket) > 16'000'000 / uint64_t(w.endBin - w.firstBin)) return {};
    return w;
}
// Existing drawn builds keep their exact generations. Other requested spans
// are formed solely from bodies currently held by this client's ChunkStore.
std::shared_ptr<const SpanSet> wallsPicture(const WallScanRequest& r, const SpanSet& drawn,
    const std::optional<ChunkAvailability>& availability, ChunkStore& store) {
    auto out = std::make_shared<SpanSet>(drawn);
    const auto window = wallWindow(r);
    if (!window || !availability) return out;
    const auto range = tiles::tilesCovering(std::max(window->from, drawn.availableStartMs),
                                          std::min(window->to, drawn.availableEndMs), r.tfMs);
    for (auto tile = range.first; tile < range.end; ++tile) {
        if (std::any_of(out->spans.begin(), out->spans.end(), [&](const auto& s) { return s.id.tfMs == r.tfMs && s.id.tile == tile; })) continue;
        SpanSnapshot span{{drawn.symbol, r.tfMs, tile}, {}, {}, true};
        for (const auto& source : availability->sources) {
            tiles::Availability time; time.oldestMs = INT64_MAX; time.minuteOldestMs = INT64_MAX;
            for (const auto& level : source.levels) {
                time.oldestMs = std::min(time.oldestMs, level.oldestMs);
                time.endMs = std::max(time.endMs, level.committedThroughMs);
                if (level.levelMs == kMinuteMs) time.minuteOldestMs = level.oldestMs;
                if (level.levelMs == kHourMs) { time.hourOldestMs = level.oldestMs; time.hourThroughMs = level.committedThroughMs; }
            }
            auto build = std::make_shared<SpanSourceBuild>();
            build->key = {span.id, source.id, std::clamp(time.oldestMs, span.id.startMs(), span.id.endMs()),
                          std::clamp(time.endMs, span.id.startMs(), span.id.endMs()), r.priceScale, 0, {}};
            std::vector<std::shared_ptr<const StoredChunk>> chunks;
            for (const auto& key : tiles::chunksFor(drawn.symbol, source.id, r.tfMs, span.id.startMs(), span.id.endMs(), time)) {
                if (auto c = store.peek(key)) {
                    build->key.generations.push_back({key.symbol, key.source, key.levelMs, key.startMs, c->generation, c->sealed});
                    chunks.push_back(c);
                }
            }
            // Clip at the advertised completed end; the compose's scan proof
            // still leaves any unheld chunk/hole loading, never a recorder gap.
            build->completeEndMs = recording::floorDiv(build->key.availableEndMs, r.tfMs) * r.tfMs;
            build->commonUnits = tiles::commonUnitsIn(chunks, span.id.startMs(), span.id.endMs(), r.tfMs);
            if (!build->commonUnits && source.latestGrid) build->commonUnits = source.latestGrid->rowTickUnits;
            std::sort(build->key.generations.begin(), build->key.generations.end());
            span.sources.push_back({source.id, build});
        }
        std::stable_sort(span.sources.begin(), span.sources.end(), [](const auto& a, const auto& b) {
            return a.build->commonUnits > b.build->commonUnits;
        });
        out->spans.push_back(std::move(span));
    }
    return out;
}
}

heatmap_window::WallsSnapshot scanWalls(const WallScanRequest& request, const SpanSet& picture,
    const LiveSnapshot* live, ChunkStore& store, LabelWindowBuilder& builder) {
    heatmap_window::WallsSnapshot out; out.gpuRenderer = true;
    const auto window = wallWindow(request);
    if (!window) { out.status = 422; return out; }
    const auto& w = *window;
    out.bandTick = w.tick; out.rangeStartMs = w.from; out.rangeEndMs = w.to;
    out.rangePriceMin = w.lo; out.rangePriceMax = w.hi;
    out.loadedStartMs = picture.availableStartMs; out.loadedEndMs = picture.availableEndMs;
    const auto better = [](const auto& a, const auto& b) {
        if (a.qty != b.qty) return a.qty > b.qty;
        if (a.bucketStartMs != b.bucketStartMs) return a.bucketStartMs < b.bucketStartMs;
        if (a.priceLow != b.priceLow) return a.priceLow < b.priceLow;
        return a.ask < b.ask;
    };
    // Finish one price stripe across time before ranking it. Even a one-column,
    // 16M-row request retains at most 500 level accumulators, not 16M map nodes.
    std::vector<bool> recorded(size_t(w.endBucket - w.firstBucket), false);
    for (auto bin = w.firstBin; bin < w.endBin;) {
        const auto count = uint32_t(std::min<int64_t>(w.endBin - bin, 250));
        std::map<std::pair<int64_t, bool>, heatmap_window::Wall> levels;
        for (auto bucket = w.firstBucket; bucket < w.endBucket;) {
            const auto columns = uint32_t(std::min<int64_t>(64, w.endBucket - bucket));
            LabelRequest q{0, picture.version, live ? live->version : 0, request.tfMs, w.units, request.priceScale,
                           bucket, bin, columns, count, {}, false};
            const auto labels = builder.build(q, picture, live, store);
            for (uint32_t x = 0; x < columns; ++x) recorded[size_t(bucket - w.firstBucket) + x] = recorded[size_t(bucket - w.firstBucket) + x] ||
                labels->columnStates[x] == BucketState::Present;
            for (uint32_t y = 0; y < count; ++y) for (uint32_t x = 0; x < columns; ++x) {
                const auto& cell = labels->cells[size_t(y) * columns + x];
                const auto state = tiles::cellState(cell.word);
                if (state == tiles::kCellVeil) out.unknownRows = true;
                if (state != tiles::kCellValid || !(cell.word & 0x7fff) || !(cell.value > 0) ||
                    !std::isfinite(cell.value) || cell.value < request.query.minQty) continue;
                const auto absoluteBin = bin + count - 1 - y;
                const double low = absoluteBin * w.tick, high = (absoluteBin + 1) * w.tick;
                const bool ask = (cell.word & 0x8000) != 0;
                const auto time = (bucket + x) * request.tfMs;
                const auto notional = cell.value * ((low + high) / 2);
                if (!std::isfinite(notional)) continue;
                auto [it, added] = levels.try_emplace({absoluteBin, ask});
                auto& wall = it->second;
                if (added) {
                    wall.priceLow = low; wall.priceHigh = high; wall.ask = ask;
                    wall.firstSeenMs = time;
                }
                if (added || cell.value > wall.qty) {
                    wall.qty = cell.value; wall.notional = notional; wall.bucketStartMs = time;
                    wall.forming = labels->formingColumns[x];
                }
                wall.meanQty += cell.value; wall.lastSeenMs = time; ++wall.columns;
            }
            bucket += columns;
        }
        for (auto& [key, wall] : levels) out.walls.push_back(wall);
        const auto keep = std::min(out.walls.size(), size_t(request.query.limit));
        std::partial_sort(out.walls.begin(), out.walls.begin() + keep, out.walls.end(), better);
        out.walls.resize(keep);
        bin += count;
    }
    for (const bool present : recorded) { out.recordedColumns += present; out.missingColumns += !present; }
    for (auto& wall : out.walls) wall.meanQty = out.recordedColumns ? wall.meanQty / out.recordedColumns : 0;
    return out;
}

struct HeatmapCellQuery::Work {
    LabelWindowBuilder builder;
};
HeatmapCellQuery::HeatmapCellQuery(ChunkStore& store, ChunkFetcher& fetcher, SpanSourceCache& cache,
                                 ChunkFetcher::ChartId chart, QObject* parent)
    : QObject(parent), store_(store), fetcher_(fetcher), cache_(cache), chart_(chart), work_(std::make_shared<Work>()) {
    connect(&cache_, &SpanSourceCache::settled, this, &HeatmapCellQuery::pump, Qt::QueuedConnection);
    connect(&cache_, &SpanSourceCache::capacityFreed, this, &HeatmapCellQuery::pump, Qt::QueuedConnection);
    connect(&cache_, &SpanSourceCache::overCeiling, this, &HeatmapCellQuery::shed, Qt::QueuedConnection);
    connect(&cache_, &SpanSourceCache::budgetsChanged, this, [this] { shed(); pump(); }, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::chunkStored, this, [this] { pump(); }, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::chunkFailed, this, [this](auto key, auto code, auto message) {
        const bool ours = std::any_of(rewant_.begin(), rewant_.end(), [&](const auto& g) {
            return ChunkKey{g.symbol, g.source, g.levelMs, g.startMs} == key;
        });
        if (waiting_ && ours) { cancel(); emit queryFailed(code + ": " + message); }
    }, Qt::QueuedConnection);
}
HeatmapCellQuery::~HeatmapCellQuery() {
    fetcher_.release(chart_);
    cache_.releaseQuery(this);
}
std::shared_ptr<const LabelCells> HeatmapCellQuery::latestLabels() const {
    std::scoped_lock lock(mutex_);
    return latest_;
}
void HeatmapCellQuery::cancel() {
    ++generation_; pending_.reset(); rewant_.clear(); waiting_ = false;
    work_ = std::make_shared<Work>(); // a running job owns its previous worker state
    retainedBytes_ = 0;
    { std::scoped_lock lock(mutex_); latest_.reset(); }
    fetcher_.release(chart_); cache_.releaseQuery(this);
}
void HeatmapCellQuery::shed() {
    if (cache_.committedCpuBytes() <= cache_.cpuCeiling()) return;
    cancel(); // optional labels shed before the visible picture
}
void HeatmapCellQuery::requestLabels(LabelRequest request, std::shared_ptr<const SpanSet> spans,
                                     std::shared_ptr<const LiveSnapshot> live) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!spans) { cancel(); return; }
    if (request.serial < latestSerial_) return;
    latestSerial_ = request.serial;
    if (request.tfMs < kMinuteMs || request.tfMs > kDayMs || request.tfMs % kMinuteMs ||
        request.firstBucket < 0 || request.firstBucket > INT64_MAX / request.tfMs - request.columns ||
        !request.rows || !request.columns || uint64_t(request.rows) * request.columns > 16000) {
        cancel(); emit queryFailed(QStringLiteral("invalid label window")); return;
    }
    if (pending_ && pending_->labels == request && pending_->spans == spans && pending_->live == live) return;
    pending_ = Request{std::move(request), std::move(spans), std::move(live), ++generation_};
    rewant_.clear(); waiting_ = false;
    if (!running_) fetcher_.release(chart_);
    pump();
}
void HeatmapCellQuery::scanWalls(WallScanRequest request, std::shared_ptr<const SpanSet> spans,
    std::shared_ptr<const LiveSnapshot> live, QObject* context,
    std::function<void(heatmap_window::WallsSnapshot)> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!context) return;
    if (!spans || !wallWindow(request) || walls_.size() >= 8) {
        heatmap_window::WallsSnapshot result;
        result.gpuRenderer = true;
        result.status = !wallWindow(request) ? 422 : 503;
        QMetaObject::invokeMethod(context, [completion = std::move(completion), result] { completion(result); }, Qt::QueuedConnection);
        return;
    }
    spans = wallsPicture(request, *spans, fetcher_.availability(spans->symbol), store_);
    walls_.push_back({request, std::move(spans), std::move(live), context, std::move(completion)});
    pump();
}
void HeatmapCellQuery::pump() {
    if (running_) return;
    const bool wall = !walls_.empty();
    if (!wall && !pending_) return;
    if (wall && waiting_) {
        // A wall scan must not inherit or strand a label's outstanding wants.
        // Keep the pending label; it rechecks held generations after the scan.
        waiting_ = false;
        rewant_.clear();
        fetcher_.release(chart_);
    }
    if (!wall && waiting_) {
        for (const auto& g : rewant_) {
            const auto c = store_.cached({g.symbol, g.source, g.levelMs, g.startMs});
            if (!c || c->generation != g.generation) return;
        }
        waiting_ = false;
    }
    const auto picture = wall ? walls_.front().spans : pending_->spans;
    const auto live = wall ? walls_.front().live : pending_->live;
    const auto request = pending_.value_or(Request{});
    const auto task = wall ? walls_.front() : WallTask{};
    const auto window = wall ? wallWindow(task.query) : std::optional<WallWindow>{};
    const int64_t from = wall ? window->from : request.labels.firstBucket * request.labels.tfMs;
    const int64_t to = wall ? window->to : (request.labels.firstBucket + request.labels.columns) * request.labels.tfMs;
    std::unordered_set<ChunkKey, ChunkKeyHash> seen;
    std::vector<ChunkBytes> keys;
    std::vector<ChunkKey> wants;
    std::vector<std::shared_ptr<const StoredChunk>> held;
    for (const auto& span : picture->spans) for (const auto& source : span.sources) if (source.build)
        for (const auto& g : source.build->key.generations) {
            const ChunkKey key{g.symbol, g.source, g.levelMs, g.startMs};
            if (g.startMs >= to || g.startMs + chunkSpanMs(g.source, g.levelMs) <= from || !seen.insert(key).second) continue;
            if (auto c = store_.peek(key); c && c->generation == g.generation) {
                keys.push_back({key, c->bytes, true}); held.push_back(c);
                if (!wall) wants.push_back(key);
            }
        }
    for (const auto& g : rewant_) {
        const ChunkKey key{g.symbol, g.source, g.levelMs, g.startMs};
        if (seen.insert(key).second) keys.push_back({key, 4ull << 20, false});
        wants.push_back(key);
    }
    if (!cache_.tryCommitQuery(this, keys, retainedBytes_)) {
        if (wall) {
            walls_.pop_front();
            if (task.context) QMetaObject::invokeMethod(task.context, [task] {
                heatmap_window::WallsSnapshot result; result.gpuRenderer = true; result.status = 503;
                task.completion(result);
            }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, &HeatmapCellQuery::pump, Qt::QueuedConnection);
        }
        return;
    }
    if (!wall) fetcher_.want(chart_, wants, SpanRank{SpanTier::Label, 0}.fetchPriority());
    struct Result {
        std::shared_ptr<const LabelCells> labels;
        heatmap_window::WallsSnapshot walls;
        size_t bytes = 0;
    };
    auto result = std::make_shared<Result>();
    const auto work = work_;
    constexpr size_t scratchReservation = 64ull << 20;
    auto job = [work, result, picture, live, request, task, wall, held = std::move(held), store = &store_] {
        try {
            if (wall) result->walls = heatmap::scanWalls(task.query, *picture, live.get(), *store, work->builder);
            else result->labels = work->builder.build(request.labels, *picture, live.get(), *store);
        } catch (...) {
            // A rejected window still leaves the worker's previous cache alive.
            result->bytes = work->builder.bytes();
            throw;
        }
        result->bytes = work->builder.bytes();
        if (result->labels) result->bytes += sizeof(LabelCells) + result->labels->cells.capacity() * sizeof(LabelCell);
    };
    auto done = [this, result, request, task, work, wall](QString error) {
        running_ = false;
        fetcher_.release(chart_);
        const bool current = work == work_;
        if (current) {
            retainedBytes_ = result->bytes;
            if (wall || !result->labels) {
                const auto labels = latestLabels();
                if (labels) retainedBytes_ += sizeof(LabelCells) + labels->cells.capacity() * sizeof(LabelCell);
            }
        }
        if (!cache_.tryCommitQuery(this, {}, retainedBytes_)) { cancel(); }
        if (!error.isEmpty()) {
            sLog_Warning("Heatmap cell query failed chart=" << chart_ << " error=" << error);
            emit queryFailed(error);
        }
        if (wall) {
            if (!error.isEmpty()) result->walls.status = 503;
            if (task.context) QMetaObject::invokeMethod(task.context, [task, result] { task.completion(result->walls); }, Qt::QueuedConnection);
        } else if (current && request.generation == generation_) {
            if (error.isEmpty() && result->labels && result->labels->missing.empty()) {
                { std::scoped_lock lock(mutex_); latest_ = result->labels; }
                pending_.reset(); rewant_.clear(); waiting_ = false;
                emit labelsChanged();
            } else if (error.isEmpty() && result->labels) {
                rewant_.clear();
                std::vector<ChunkBytes> missing;
                std::vector<ChunkKey> wants;
                std::unordered_set<ChunkKey, ChunkKeyHash> unique;
                for (const auto& g : result->labels->missing) {
                    const ChunkKey key{g.symbol, g.source, g.levelMs, g.startMs};
                    if (!g.sealed || store_.generationOf(key) != g.generation || !unique.insert(key).second) continue;
                    rewant_.push_back(g); wants.push_back(key); missing.push_back({key, 4ull << 20, false});
                }
                if (!rewant_.empty() && cache_.tryCommitQuery(this, std::move(missing), retainedBytes_)) {
                    waiting_ = true;
                    fetcher_.want(chart_, wants, SpanRank{SpanTier::Label, 0}.fetchPriority());
                } else { pending_.reset(); rewant_.clear(); }
            } else pending_.reset();
        }
        pump();
    };
    if (cache_.requestQuery(std::move(keys), scratchReservation, this, std::move(job), std::move(done))) {
        running_ = true;
        if (wall) walls_.pop_front();
    } else if (wall && cache_.committedCpuBytes() + scratchReservation > cache_.cpuCeiling()) {
        walls_.pop_front();
        if (task.context) QMetaObject::invokeMethod(task.context, [task] {
            heatmap_window::WallsSnapshot result; result.gpuRenderer = true; result.status = 503;
            task.completion(result);
        }, Qt::QueuedConnection);
        cache_.tryCommitQuery(this, {}, retainedBytes_);
        QMetaObject::invokeMethod(this, &HeatmapCellQuery::pump, Qt::QueuedConnection);
    }
}
} // namespace heatmap
