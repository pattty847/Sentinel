#pragma once
// Benchmark/inspection harness for the production heatmap GPU path: a plain
// QQuickItem hosting heatmap::gpu::HeatmapRenderNode in the normal scene graph,
// fed by heatmap::SparseColumns composed on a worker thread.
// Slice T (docs/research/2026-09-heatmap-interaction-spec.md, experiments E1-E3):
// a column is exactly the selected timeframe (no auto-timeframe); time zoom-out
// clamps at one column per physical pixel; tick is Auto (minRowPx + hysteresis)
// or Manual (locked preset, remembered per symbol and timeframe, price zoom-out
// clamped at one row per physical pixel, never coarsened).
// Slice B1 (docs/research/2026-09-b1-whole-chunk-benchmark.md): the preparation
// mode ("prep") of real recordings, all fed from one process-wide chunk store:
//   full        - the S4/T lab path: the whole requested range as one source
//                 (no row clip); screen-sized grid, re-bin on leaving the guard.
//   viewport    - V: a source clipped to the view plus one view each side (time
//                 and price), rebuilt when the view leaves it; screen-sized grid.
//   whole-chunk - W: 64-column tiles binned over their whole useful price extent
//                 on the GPU into cached render-ready buffers; pan/zoom = mapping.
//   whole-chunk-cpu - W with the tiles' cells built on the CPU (binColumn).
//   hybrid      - the W tile spans' GpuSources stay resident on the GPU (whole
//                 price extent, uploaded once per span and timeframe); each tile
//                 bins only the rows around the view (one view height each
//                 side), re-binned in place by a compute pass when the view
//                 leaves them or the tick changes.
#include "LabChunks.hpp"
#include "LabSources.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "heatmap/HeatmapTiles.hpp"
#include "render/heatmap/HeatmapRenderNode.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"
#include <QElapsedTimer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace lab {
enum class PrepMode { Full, Viewport, WholeChunkGpu, WholeChunkCpu, Hybrid };
QString prepModeName(PrepMode mode);
std::optional<PrepMode> parsePrepMode(const QString &name);

class LabItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int timeframeMinutes READ timeframeMinutes WRITE setTimeframeMinutes NOTIFY timeframeChanged)
    Q_PROPERTY(bool manualMode READ manualMode WRITE setManualMode NOTIFY tickChanged)
    Q_PROPERTY(double manualTick READ manualTick WRITE setManualTick NOTIFY tickChanged)
    Q_PROPERTY(double hysteresis READ hysteresis WRITE setHysteresis NOTIFY tickChanged)
    Q_PROPERTY(double minRowPx READ minRowPx WRITE setMinRowPx NOTIFY tickChanged)
    Q_PROPERTY(bool crossfade READ crossfade WRITE setCrossfade NOTIFY tickChanged)
    Q_PROPERTY(QVariantList offeredTicks READ offeredTicks NOTIFY presetsChanged)
    Q_PROPERTY(QString prepMode READ prepModeString WRITE setPrepModeString NOTIFY prepModeChanged)
public:
    static constexpr double kCrossfadeMs = 150; // spec rule 8 (owner chose it after E2)
    // B1: one per-frame upload budget for every mode (was 2 MiB in S4/T).
    static constexpr uint64_t kDefaultUploadBudgetBytes = 8ull << 20;
    static constexpr uint64_t kDefaultTileBudgetBytes = 256ull << 20; // W render-ready tiles per chart
    static constexpr int64_t kPrefetchTiles = 1;                      // W: tiles beyond the view, each side
    explicit LabItem(QQuickItem *parent = nullptr);
    ~LabItem() override;
    QString status() const { return status_; }
    int timeframeMinutes() const { return timeframeMinutes_; }
    bool manualMode() const { return manualMode_; }
    double manualTick() const { return manualTick_; }
    double hysteresis() const { return hysteresis_; }
    double minRowPx() const { return minRowPx_; }
    bool crossfade() const { return crossfade_; }
    QVariantList offeredTicks() const { return offeredTicks_; }
    PrepMode prep() const { return prepMode_; }
    QString prepModeString() const { return prepModeName(prepMode_); }
    void setPrepModeString(const QString &mode);
    void setPrepMode(PrepMode mode);
    void setTimeframeMinutes(int minutes);
    // true: lock the remembered tick for (symbol, timeframe), else the tick drawn now.
    void setManualMode(bool manual);
    // Locks this preset (switches to Manual) and remembers it for (symbol, timeframe).
    void setManualTick(double tick);
    void setHysteresis(double h);
    void setMinRowPx(double px);
    void setCrossfade(bool enabled);
    // Remember Manual ticks across runs (QSettings). Off for headless runs.
    void setPersistTickMemory(bool persist);
    // Once the first complete source is shown: zoom price so one commonTick() row
    // of the data in view is this many physical pixels tall (0 = off).
    void setInitialRowPx(double px) { initialRowPx_ = px; }
    void setUploadBudgetBytes(uint64_t bytes) { uploadBudget_ = std::max<uint64_t>(bytes, 1); }
    void setTileBudgetBytes(uint64_t bytes);
    Q_INVOKABLE void loadReal(int hours, const QString &layer);
    Q_INVOKABLE void loadSynthetic(int count);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoom(double steps, bool priceOnly, double anchorX, double anchorY);
    // Wheel input (angle deltas in 1/8 degree). priceOnly (Shift) scales the price
    // axis only; macOS delivers Shift+wheel as horizontal scroll, so with Shift
    // whichever axis moved is used.
    Q_INVOKABLE void wheelZoom(double angleX, double angleY, bool priceOnly, double anchorX, double anchorY);
    // Price zoom about the view centre: one commonTick() row of the data in view
    // is `px` physical pixels tall (Manual clamps still apply).
    Q_INVOKABLE void zoomToRowPx(double px);
    Q_INVOKABLE QVariantMap metrics() const;
    Q_INVOKABLE bool saveScreenshot(const QString &path = {});
    // Manual only: why some columns in view draw the veil instead of the locked
    // tick (empty when every column in view can build it, or in Auto).
    QString resolutionIndicator() const;
    double devicePixelRatio() const;
    // True once the newest requested source (all load phases) is uploaded and drawn.
    // In the V/W modes: the whole view is drawn from the prepared representation
    // of the current timeframe and tick (no loading hatch from missing prep, no
    // stale or held picture, no build in flight for the view).
    bool settled() const;
    // Scripted sessions (B1 bench).
    heatmap::gpu::ViewWindow view() const { return view_; }
    void setView(const heatmap::gpu::ViewWindow &view);
    // V/W: the view to show once the recording's availability is known, instead
    // of the default (newest `hours`, +-2 % around the recent price).
    void setInitialView(const heatmap::gpu::ViewWindow &view) { pendingView_ = view; }
    // Re-reads availability and reloads the newest (open) chunk as a new revision,
    // as if the server had sent a revised live chunk.
    Q_INVOKABLE void reviseNewestChunk();
    bool hasContent() const;
signals:
    void statusChanged();
    void timeframeChanged();
    void tickChanged();
    void presetsChanged();
    void prepModeChanged();
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
private:
    struct WTile {
        heatmap::gpu::TileRef ref;
        std::vector<std::pair<heatmap::ChunkKey, uint64_t>> chunkGenerations;
        bool clipped = false;
        bool wasResident = false; // the node reported it resident at least once
    };
    using TileBase = std::tuple<int64_t, int64_t, int64_t>; // tfMs, tickUnits, tile
    heatmap::gpu::ViewWindow view_;
    LabSource source_;
    bool loading_ = false;
    std::shared_ptr<heatmap::gpu::HeatmapRenderStats> stats_ = std::make_shared<heatmap::gpu::HeatmapRenderStats>();
    std::shared_ptr<heatmap::gpu::HeatmapTileStats> tileStats_ = std::make_shared<heatmap::gpu::HeatmapTileStats>();
    std::shared_ptr<std::atomic<uint64_t>> loadGeneration_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::mutex> loadMutex_ = std::make_shared<std::mutex>();
    QString status_ = QStringLiteral("Select a source");
    int realHours_ = 24;
    QString realLayer_ = QStringLiteral("near");
    bool realMode_ = false;
    int syntheticCount_ = 0;
    int timeframeMinutes_ = 1;
    bool manualMode_ = false;
    double manualTick_ = 0;
    bool explicitTickPending_ = false; // set before the symbol is known: store it on accept
    double hysteresis_ = 0.25, minRowPx_ = 2;
    bool crossfade_ = true; // owner choice after E2 (spec rule 8)
    bool persistTickMemory_ = false;
    double initialRowPx_ = 0;
    heatmap::ManualTickMemory tickMemory_;
    QVariantList offeredTicks_;
    QElapsedTimer launched_, lastFrame_;
    double frameMs_ = 0, firstFrameMs_ = 0;
    QMetaObject::Connection frameConnection_;
    uint64_t uploadBudget_ = kDefaultUploadBudgetBytes;
    PrepMode prepMode_ = PrepMode::Full;
    int shownNode_ = -1; // 0 = HeatmapRenderNode, 1 = HeatmapTileNode

    // ---- chunked modes (V, W): shared
    bool haveAvailability_ = false;
    heatmap::tiles::Availability availability_;
    uint64_t chunkSerial_ = 0; // invalidates worker results after a reload
    std::set<std::string> chunkLoadsInFlight_; // "levelMs/startMs"
    // ---- V
    ViewportSource vSource_;
    bool vInFlight_ = false;
    uint64_t vBuilds_ = 0, vUploadBytes_ = 0;
    std::optional<heatmap::gpu::ViewWindow> pendingView_;
    double vLastRequestMs_ = 0; // wall time from request to accepted source
    // ---- W
    heatmap::tiles::ByteLru<heatmap::tiles::TileKey, WTile, heatmap::tiles::TileKeyHash> wTiles_{kDefaultTileBudgetBytes};
    std::map<TileBase, heatmap::tiles::TileKey> wLatest_, wPrevious_;
    std::set<TileBase> wInFlight_;
    // Prefetch tiles the budget evicted under the current plan (not re-requested
    // until the plan or the budget changes; see updateWholeChunk).
    using PlanKey = std::tuple<int64_t, int64_t, int64_t, int64_t>; // tfMs, key tick, visible first/end
    PlanKey wPlan_{};
    std::set<TileBase> wBudgetEvicted_;
    // The node's drawn ids and resident count at the last eviction pass: while
    // over budget, a change re-runs it (updatePaintNode queues evictTiles()).
    std::vector<uint64_t> wEvictDrawn_;
    size_t wEvictResident_ = 0;
    bool wEvictQueued_ = false;
    int64_t wAutoUnits_ = 0, wTickUnits_ = 0;
    int64_t wPrevTickUnits_ = 0; // hybrid: the tick before the last change (held/fading draws)
    uint64_t wBuilds_ = 0, wIntermediateHits_ = 0, wClipped_ = 0, wLostTiles_ = 0;
    double wLastBuildMs_ = 0;
    size_t wCpuBytes_ = 0; // render-ready data not yet handed to the GPU

    int64_t tfMs() const { return int64_t(timeframeMinutes_) * 60'000; }
    std::string symbol() const;
    double priceScale() const;
    std::string layer() const { return realLayer_.toStdString(); }
    bool chunked() const { return realMode_ && prepMode_ != PrepMode::Full; }
    bool wholeChunk() const {
        return chunked() && (prepMode_ == PrepMode::WholeChunkGpu || prepMode_ == PrepMode::WholeChunkCpu ||
                             prepMode_ == PrepMode::Hybrid);
    }
    void acceptTile(const TileBase &base, const TileBuild &built);
    // Enforces the W tile budget (protection rules inside); records budget-evicted prefetch.
    void evictTiles();
    // Hybrid tiles are cached per span (their resident source serves every tick):
    // the cache key has tick 0 and the renderer sees one tile id per span and tick.
    int64_t keyTick(int64_t tickUnits) const { return prepMode_ == PrepMode::Hybrid ? 0 : tickUnits; }
    static uint64_t tickTileId(uint64_t entryId, int64_t tickUnits) {
        return (entryId << 32) | uint64_t(uint32_t(tickUnits));
    }
    void accept(LabSource source, bool preserveView, bool final);
    void reload(bool preserveView);
    void clampView();
    void rememberTick();
    void restoreTick();
    double fallbackManualTick() const;
    void loadTickMemory();
    // A view, timeframe, tick or mode change: re-plan the prepared data, repaint.
    void viewChanged();
    void startChunked(bool preserveView);
    void resetChunkedState();
    void updateViewport();
    void updateWholeChunk();
    void requestChunks(double loMs, double hiMs);
    void refreshChunkPresets();
    bool viewportCovers() const;
    bool tileStale(const WTile &tile) const;
    uint64_t targetKey() const;
    double finestTick() const;
    double commonTickInViewAny() const;
    QVariantMap prepMetrics() const;
};
} // namespace lab
