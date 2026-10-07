#pragma once
#include "HeatmapSourceController.hpp"
#include "heatmap/TimeComposer.hpp"
#include "heatmap/DrawPieces.hpp"
#include "HeatmapWalls.hpp"
#include <QPointer>
#include <array>
#include <list>
#include <deque>

namespace heatmap {
using HeldChunks = std::unordered_map<ChunkKey, std::shared_ptr<const StoredChunk>, ChunkKeyHash>;

struct LabelRequest {
    uint64_t serial = 0, spanVersion = 0, liveVersion = 0;
    int64_t tfMs = 0, tickUnits = 0;
    double priceScale = 100;
    int64_t firstBucket = 0, firstBin = 0; // bottom bin; output rows descend
    uint32_t columns = 0, rows = 0;
    std::string asset;
    bool formatText = true;
    bool operator==(const LabelRequest &) const = default;
};
// Label text: at most three significant digits with k/M/B/T, trailing zeros
// dropped, "$" for USD; the asset amount is the number only ("$11k", "$1.24M",
// "$1.2M", "12.5"; owner 2026-10-02). Empty for a value <= 0 or not finite.
void formatLabelAmount(std::array<char, 48> &out, double value, bool usd);
struct LabelCell {
    uint32_t word = tiles::cellWord(tiles::kCellNoData);
    double value = 0; // exact dominant-side value of the winning source
    std::array<char, 48> usd{}, asset{};
};
struct LabelCells {
    LabelRequest key;
    tiles::TileGrid grid;
    std::vector<LabelCell> cells; // row-major, highest price first
    std::vector<BucketState> columnStates;
    std::vector<bool> formingColumns;
    // S7b drawn-picture identity: per column, whether the live window (not a span)
    // supplied it, and each span's spanContentId (tile, id).
    std::vector<bool> liveColumns;
    std::vector<std::pair<int64_t, uint64_t>> spanContent;
    std::vector<ChunkGeneration> missing; // never substituted with newer generations
    uint64_t composedWindows = 0, cacheHits = 0;
    uint64_t reusedHistoryColumns = 0;
};

// Worker-owned, byte-bounded cache of tick-free compositions. A tick change
// reuses any containing time/price window of the same drawn source generation.
// It never retains decoded chunks or upload images. One worker uses it at a time.
class LabelWindowBuilder {
public:
    explicit LabelWindowBuilder(size_t maxBytes = 32ull << 20) : maxBytes_(maxBytes) {}
    std::shared_ptr<const LabelCells> build(const LabelRequest &request, const SpanSet &spans,
                                           const LiveSnapshot *live, ChunkStore &store, const HeldChunks &held = {});
    void clear() { windows_.clear(); bytes_ = 0; previous_.reset(); previousPieces_.clear(); }
    size_t bytes() const { return bytes_ + (previous_ ? previous_->cells.capacity() * sizeof(LabelCell) : 0); }
private:
    struct Window {
        SpanSourceKey key;
        int64_t fromMs = 0, toMs = 0;
        ComposeOptions::PriceClip price;
        std::shared_ptr<const SparseColumns> columns;
        size_t bytes = 0;
    };
    std::list<Window> windows_;
    size_t maxBytes_ = 0, bytes_ = 0;
    std::shared_ptr<const LabelCells> previous_;
    std::vector<DrawPiece> previousPieces_;
    std::shared_ptr<const SparseColumns> composeWindow(const SpanSourceKey &, int64_t fromMs, int64_t toMs,
        ComposeOptions::PriceClip price, ChunkStore &, LabelCells &, const HeldChunks &);
};

struct WallScanRequest {
    heatmap::WallQuery query;
    int64_t tfMs = 0, drawnTickUnits = 0;
    double priceScale = 100;
    double timeLoMs = 0, timeHiMs = 0, priceLo = 0, priceHi = 0;
};
// Same cell oracle and fill pass as labels; bounded batches, no text formatting.
heatmap::WallsSnapshot scanWalls(const WallScanRequest &, const SpanSet &, const LiveSnapshot *,
                                        ChunkStore &, LabelWindowBuilder &, const HeldChunks &held = {});

// Per controller; lives on heatmap-data, uses the cache's bounded worker pool.
// Callers pass the immutable *drawn* picture, including held sources. Only the
// newest request publishes; read latestLabels on any thread, connect queued.
class HeatmapCellQuery final : public QObject {
    Q_OBJECT
public:
    HeatmapCellQuery(ChunkStore &, ChunkFetcher &, SpanSourceCache &, ChunkFetcher::ChartId, QObject *parent = nullptr);
    ~HeatmapCellQuery() override;
    void requestLabels(LabelRequest, std::shared_ptr<const SpanSet>, std::shared_ptr<const LiveSnapshot> = {});
    std::shared_ptr<const LabelCells> latestLabels() const;
    // Client-held chunks only, including cached history outside the drawn view.
    // Completion is queued to context; no label request or periodic scan needed.
    void scanWalls(WallScanRequest, std::shared_ptr<const SpanSet>, std::shared_ptr<const LiveSnapshot>,
                   QObject *context, std::function<void(heatmap::WallsSnapshot)> completion);
    void cancel();
signals:
    void labelsChanged();
    void queryFailed(QString message);
private:
    struct Work;
    struct Request {
        LabelRequest labels;
        std::shared_ptr<const SpanSet> spans;
        std::shared_ptr<const LiveSnapshot> live;
        uint64_t generation = 0;
    };
    struct WallTask {
        WallScanRequest query;
        std::shared_ptr<const SpanSet> spans;
        std::shared_ptr<const LiveSnapshot> live;
        QPointer<QObject> context;
        std::function<void(heatmap::WallsSnapshot)> completion;
    };
    ChunkStore &store_;
    ChunkFetcher &fetcher_;
    SpanSourceCache &cache_;
    ChunkFetcher::ChartId chart_;
    std::shared_ptr<Work> work_;
    std::optional<Request> pending_;
    std::deque<WallTask> walls_;
    uint64_t generation_ = 0;
    uint64_t latestSerial_ = 0;
    bool running_ = false, waiting_ = false;
    size_t retainedBytes_ = 0;
    mutable std::mutex mutex_;
    std::shared_ptr<const LabelCells> latest_;
    std::vector<ChunkGeneration> rewant_;
    void pump();
    void shed();
};
} // namespace heatmap
