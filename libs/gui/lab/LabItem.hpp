#pragma once
// The GPU heatmap lab chart, on the production path the main chart takes in S6
// (slice S5c, docs/research/2026-09-s5-plan.md):
//   LocalChunkTransport -> ChunkFetcher -> ChunkStore -> HeatmapSourceController
//   (all on the "heatmap-data" thread, LabData) -> SpanSet -> HeatmapTileNode.
// The item owns the view and the tick policy (interaction spec,
// docs/research/2026-09-heatmap-interaction-spec.md):
// - a column is exactly the selected timeframe; time zoom-out clamps at one
//   column per physical pixel;
// - Auto tick (default): the smallest preset whose rows are at least minRowPx
//   tall that the data builds on every row in view (the SpanSet's resolution
//   summary), with hysteresis h; Manual: a locked preset, remembered per symbol
//   and timeframe, price zoom-out clamped at one row per physical pixel, never
//   coarsened (unbuildable rows veil and the resolution indicator names them);
// - the tick is chosen in updatePaintNode (GUI blocked) and handed to the node in
//   the same frame, then posted to the controller;
// - a 150 ms crossfade on tick change (a setting turns it into a hard switch).
// E4 (near-band seam): bandEdges() gives the edges of the finest source's
// coverage band in view, drawn over the chart when showBandEdges is on.
#include "LabData.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"
#include <QElapsedTimer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>

namespace lab {
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
    Q_PROPERTY(bool showBandEdges READ showBandEdges WRITE setShowBandEdges NOTIFY bandEdgesChanged)
public:
    static constexpr double kCrossfadeMs = 150; // spec rule 8 (owner chose it after E2)
    static constexpr uint64_t kDefaultUploadBudgetBytes = 8ull << 20;
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
    bool showBandEdges() const { return showBandEdges_; }
    void setTimeframeMinutes(int minutes);
    // true: lock the remembered tick for (symbol, timeframe), else the tick drawn now.
    void setManualMode(bool manual);
    // Locks this preset (switches to Manual) and remembers it for (symbol, timeframe).
    void setManualTick(double tick);
    void setHysteresis(double h);
    void setMinRowPx(double px);
    void setCrossfade(bool enabled);
    void setShowBandEdges(bool show);
    // Remember Manual ticks across runs (QSettings). Off for headless runs.
    void setPersistTickMemory(bool persist);
    // Once the first spans are drawn: zoom price so one common-tick row of the data
    // in view is this many physical pixels tall (0 = off).
    void setInitialRowPx(double px) { initialRowPx_ = px; }
    void setUploadBudgetBytes(uint64_t bytes) { uploadBudget_ = std::max<uint64_t>(bytes, 1); }
    // Per-chart GPU cap (node and controller); default HeatmapBudgets::gpuPerChart.
    void setGpuCapBytes(uint64_t bytes);
    // Starts the chart on kSymbol: the newest `hours` (clamped to one column per
    // pixel), price +-2 % around the recent mid once the first spans are built.
    Q_INVOKABLE void loadReal(int hours);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoom(double steps, bool priceOnly, double anchorX, double anchorY);
    // Wheel input (angle deltas in 1/8 degree). priceOnly (Shift) scales the price
    // axis only; macOS delivers Shift+wheel as horizontal scroll, so with Shift
    // whichever axis moved is used.
    Q_INVOKABLE void wheelZoom(double angleX, double angleY, bool priceOnly, double anchorX, double anchorY);
    // Price zoom about the view centre: one common-tick row of the data in view is
    // `px` physical pixels tall (Manual clamps still apply).
    Q_INVOKABLE void zoomToRowPx(double px);
    Q_INVOKABLE QVariantMap metrics() const;
    Q_INVOKABLE bool saveScreenshot(const QString &path = {});
    // Columns and price ranges in view that draw the veil at the tick (no source
    // builds it there); empty when none.
    Q_INVOKABLE QString resolutionIndicator() const;
    // E4: runs of columns in view with the finest source's full-coverage band, as
    // [x0, x1, yTop, yBottom] in item coordinates (price outside the view clamps).
    Q_INVOKABLE QVariantList bandEdges() const;
    double devicePixelRatio() const;
    // True once the whole view is drawn from complete content of the current
    // timeframe and tick: no loading hatch, no held, fallback or partial picture,
    // every visible span built at its latest generation and resident.
    bool settled() const;
    heatmap::gpu::ViewWindow view() const { return view_; }
    void setView(const heatmap::gpu::ViewWindow &view);
    // The view to show once the first spans are built (instead of the default).
    void setInitialView(const heatmap::gpu::ViewWindow &view) { pendingView_ = view; }
    bool hasContent() const { return loaded_; }
    int64_t tickUnits() const { return tickUnits_; }
    const heatmap::gpu::HeatmapTileStats &tileStats() const { return *tileStats_; }
    std::shared_ptr<const heatmap::SpanSet> snapshot() const { return snapshot_; }
    // Tests: the controller (lives on the data thread).
    heatmap::HeatmapSourceController *controller() const { return controller_; }
signals:
    void statusChanged();
    void timeframeChanged();
    void tickChanged();
    void presetsChanged();
    void bandEdgesChanged();
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
private:
    heatmap::gpu::ViewWindow view_;
    std::optional<heatmap::gpu::ViewWindow> pendingView_;
    bool loaded_ = false, priceKnown_ = false;
    int hours_ = 24;
    heatmap::HeatmapSourceController *controller_ = nullptr;
    std::shared_ptr<heatmap::HeatmapCapacity> capacity_;
    std::shared_ptr<const heatmap::SpanSet> snapshot_;
    std::shared_ptr<heatmap::gpu::HeatmapTileStats> tileStats_ = std::make_shared<heatmap::gpu::HeatmapTileStats>();
    QString status_ = QStringLiteral("Select a source");
    int timeframeMinutes_ = 1;
    bool manualMode_ = false;
    double manualTick_ = 0;
    bool explicitTickPending_ = false;
    double hysteresis_ = 0.25, minRowPx_ = 2;
    bool crossfade_ = true;
    bool showBandEdges_ = false;
    bool persistTickMemory_ = false;
    double initialRowPx_ = 0;
    heatmap::ManualTickMemory tickMemory_;
    QVariantList offeredTicks_;
    uint64_t uploadBudget_ = kDefaultUploadBudgetBytes;
    uint64_t gpuCap_ = heatmap::HeatmapBudgets{}.gpuPerChart;
    // Tick state (written in updatePaintNode while the GUI thread is blocked).
    int64_t tickUnits_ = 0, autoUnits_ = 0, postedTickUnits_ = -1;
    bool postedManual_ = false;
    struct TickKey {
        uint64_t version = 0;
        const heatmap::SpanSet *set = nullptr;
        heatmap::gpu::ViewWindow view;
        double heightPx = 0, minRowPx = 0, h = 0;
        int64_t current = 0;
        bool operator==(const TickKey &) const = default;
    } tickKey_;
    uint64_t tickChanges_ = 0;
    uint64_t viewSerial_ = 0, renderedSerial_ = 0; // settled() needs a frame of the current view
    QElapsedTimer launched_, lastFrame_;
    double frameMs_ = 0, firstFrameMs_ = 0;
    QMetaObject::Connection frameConnection_;
    // Controller stats, fetched asynchronously for the debug panel.
    struct ControllerStats {
        heatmap::HeatmapSourceController::Stats stats;
        bool valid = false;
    };
    std::shared_ptr<ControllerStats> controllerStats_ = std::make_shared<ControllerStats>();

    int64_t tfMs() const { return int64_t(timeframeMinutes_) * 60'000; }
    std::string symbol() const { return kSymbol; }
    double priceScale() const { return snapshot_ ? snapshot_->priceScale : 100.0; }
    void onSnapshot();
    void postView();
    int64_t chooseTick(); // Auto/Manual; GUI state only (called with the GUI thread owning or blocked)
    void clampView();
    void rememberTick();
    void restoreTick();
    double fallbackManualTick() const;
    void loadTickMemory();
    void refreshPresets();
    void initialisePrice();
    double commonTickInView() const;
    // A view, timeframe, tick or mode change: post the view, repaint.
    void viewChanged();
};
} // namespace lab
