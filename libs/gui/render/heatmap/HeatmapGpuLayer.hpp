#pragma once
#include "HeatmapCellQuery.hpp"
// The main chart's GPU heatmap layer (S6b, docs/research/2026-10-s6-plan.md
// section 2): LabItem without view ownership. UnifiedGridRenderer owns one per
// chart (GUI thread) and gives it the symbol, timeframe, the committed view of
// its GridViewState and the chart settings; the layer owns:
// - a controller handle from the process HeatmapDataService (one per chart),
// - the latest SpanSet, LiveSnapshot and ResolutionSummary,
// - the tick policy (interaction spec rules 1-4) with its TickKey cache,
//   Manual tick memory per (symbol, timeframe) and the offered presets,
// - the per-chart node frame inputs (palette, style, budgets) and HeatmapTileStats.
// chooseTickForView() and prepareFrame() run in updatePaintNode (render thread,
// GUI thread blocked): the first takes the freshest snapshot, chooses the tick
// (cached by TickKey: no work when nothing changed) and queues the tick request to
// the controller; the second fills the HeatmapTileNode frame for the raster
// camera's drawn view in the same frame. No allocation per frame beyond the lab's
// (the node frame holds shared pointers).
#include "HeatmapDataService.hpp"
#include "HeatmapPalette.hpp"
#include "HeatmapTileNode.hpp"
#include "HeatmapLabelMatch.hpp"
#include "heatmap/HeatmapChartSettings.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <QJsonObject>
#include <QVariantMap>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace heatmap::gpu {
// The label request retry (S7b): a posted request that got no result (or lost
// it to HeatmapCellQuery::cancel()) is asked again after delayMs(), which doubles
// per failure up to kMaxMs and returns to kFirstMs after a result.
struct LabelRetryPolicy {
    static constexpr int kFirstMs = 2000, kMaxMs = 30000;
    int delayMs() const { return delay_; }
    void failed() { delay_ = std::min(delay_ * 2, kMaxMs); }
    void succeeded() { delay_ = kFirstMs; }
private:
    int delay_ = kFirstMs;
};

// Median best-bid/best-ask midpoint over the newest columns (the price a fresh
// view centres on). 0 when none.
double medianRecentMid(const SparseColumns &data);

class HeatmapGpuLayer final : public QObject {
    Q_OBJECT
public:
    explicit HeatmapGpuLayer(QObject *parent = nullptr);
    ~HeatmapGpuLayer() override;

    // GUI thread. The process service must outlive the layer's controller
    // (setService(nullptr) or setActive(false) destroys it first).
    void setService(HeatmapDataService *service);
    HeatmapDataService *service() const { return service_; }
    // Active (gpu renderer): a controller exists and follows the chart. Inactive:
    // the controller is destroyed; the node goes with the chart's gpu root.
    void setActive(bool active);
    bool active() const { return active_; }
    void setSymbol(const std::string &symbol);
    void setTimeframeMs(int64_t tfMs);
    // The chart's committed view; priceKnown false while the price window is a
    // placeholder (cold start, symbol switch): nothing is drawn, no tick chosen.
    void setView(const ViewWindow &view, bool priceKnown);
    // Chart surface (logical px) and device pixel ratio: clamps and Auto tick.
    void setSurface(double widthPx, double heightPx, double dpr);
    // Chart settings. A Manual tick comes from the settings only when
    // explicitManualTick (an explicit choice, remembered for symbol/timeframe);
    // entering Manual otherwise restores the remembered tick (spec rule 2).
    void setSettings(const HeatmapChartSettings &settings, bool explicitManualTick = false);
    const HeatmapChartSettings &settings() const { return settings_; }
    void setTone(PaletteTone tone);
    // Process-wide Manual tick memory (owner decision 5), loaded by the host.
    void setTickMemory(const ManualTickMemory &memory);
    const ManualTickMemory &tickMemory() const { return tickMemory_; }

    // Render thread, GUI thread blocked (updatePaintNode), in this order each frame:
    // chooseTickForView takes the freshest snapshot and chooses the tick for the
    // continuous view (committed + drag: Auto decides on the continuous camera);
    // the chart then computes its raster camera at that tick, and prepareFrame
    // fills the node frame and posts labels for `drawnView`, the raster camera's
    // window (exactly what is on screen) over `rect`. prepareFrame returns false
    // when there is nothing to draw yet (no controller).
    // The tick it chooses is a proposal: commitProposedTick (queued to the GUI thread)
    // makes it the drawn tick and emits tickChanged, so the chart moves every dependent
    // layer before the next frame draws it.
    void chooseTickForView(const ViewWindow &view);
    // coverageGamma: 0 at rest; 2.2 while a zoom transition draws fractional pixels
    // (coverage in linear light). binView: a zoom glide's whole extent (binned once).
    bool prepareFrame(HeatmapTileNode::Frame &frame, const QRectF &rect, const ViewWindow &drawnView,
                      float coverageGamma = 0.0f, std::optional<ViewWindow> binView = std::nullopt);
    // GUI thread: the tick a frame would choose for `view` now (the same rule and
    // state as chooseTickForView; nothing changes): the zoom ladder's target tick.
    int64_t predictTickUnits(const ViewWindow &view) const;
    // GUI-thread inputs for the Auto-price fitter; candidate coverage includes
    // its fitted margin. The frame commits the decision instead of re-deciding.
    int64_t autoTickState() const { return autoUnits_; }
    bool hasCurrentResolution() const; // false during startup/symbol/timeframe transients
    // The fitted tick decision, if any (distinct from the committed/drawn tick).
    std::optional<int64_t> autoPriceTickUnits() const { return autoPriceTick_; }
    bool buildsTick(int64_t units, const ViewWindow &candidate) const;
    bool setAutoPriceTick(std::optional<int64_t> units); // true: schedule a frame even if bounds stayed
    // Render thread: the frame's drawn device pixels per row and column and the
    // continuous rows' height (device px) for metrics() (atomics; GUI-thread reads).
    void noteRaster(int rowPx, int colPx, double rowPxContinuous);

    // Spec rules 1, 2 and 9 for the chart's GridViewState (0 = none): one column
    // per physical pixel; Manual: one row per physical pixel.
    double maxTimeSpanMs() const;
    double maxPriceSpan() const;
    // The price scale is the active symbol's: false while the previous symbol's
    // snapshot is still the newest after a switch, or no snapshot exists yet, or the
    // symbol has no recorded availability (live only). The price-span limits are unknown
    // (0) until then: another symbol's scale must not size them.
    bool priceScaleCurrent() const;
    // Zoom-in floors: kMinZoomColumns columns of the timeframe; kMinZoomRows rows of
    // the Manual tick, or of the finest offered preset in Auto (0 = none yet).
    static constexpr int kMinZoomColumns = 4, kMinZoomRows = 4;
    double minTimeSpanMs() const;
    double minPriceSpan() const;
    int64_t tfMs() const { return tfMs_; }
    const std::string &symbol() const { return symbol_; }
    bool manualMode() const { return manualMode_; }
    int64_t manualTickUnits() const { return manualUnits_; }
    int64_t tickUnits() const { return tickUnits_; } // chosen for the last frame
    double priceScale() const;
    double tickPrice() const { return tickUnits_ > 0 ? fromUnits(tickUnits_, priceScale()) : 0.0; }
    // True once the whole view is drawn from complete content of the current
    // timeframe and tick (as LabItem::settled).
    bool settled() const;
    std::shared_ptr<const SpanSet> snapshot() const { return snapshot_; }
    std::shared_ptr<const LiveSnapshot> live() const { return live_; }
    // Newest live open end of the current symbol and timeframe (0: none).
    int64_t liveOpenEndMs() const;
    // Where "live" is for this symbol, independent of the live subscription (a
    // historical view drops it): the newest live open end seen, or the recording's
    // committed end from the service's availability. 0: unknown yet.
    int64_t liveAnchorMs() const;
    // Oldest recorded bucket start of this symbol from the service's availability
    // (any source and level; 0: unknown yet).
    int64_t oldestAvailableMs() const;
    // Wall-clock receive time of the newest live version (ms since epoch; 0: none).
    int64_t liveReceivedAtMs() const { return liveReceivedAtMs_; }
    // The price a fresh view centres on, from the decoded chunks of the newest
    // visible span (0 when none is decoded).
    double recentMidPrice() const;
    std::vector<int64_t> offeredTickUnits() const { return offered_; }
    QString resolutionIndicator() const;
    const HeatmapTileStats &tileStats() const { return *tileStats_; }
    // The stats every node of this layer writes (one object for the layer's life).
    std::shared_ptr<HeatmapTileStats> tileStatsPtr() const { return tileStats_; }
    HeatmapSourceController *controller() const { return controller_; }
    void scanWalls(const heatmap::WallQuery &query,
                   std::function<void(heatmap::WallsSnapshot)> completion);
    // Agent API heatmap state (plan section 5); refreshes the controller stats
    // asynchronously for the next call.
    QJsonObject state() const;
    // The telemetry panel's values (the lab table: tick, GPU node, live edge, data
    // controller, render). GUI thread, polled at 4 Hz by HeatmapTelemetryDock;
    // atomics and shared pointers only, plus the asynchronous controller stats.
    QVariantMap metrics() const;

    // Liquidity labels (S7b, plan sections 3-5). prepareFrame posts a LabelRequest
    // for the frame's picture (the chart's timeframe and the tick chosen for the
    // frame, the current SpanSet and live snapshot) only when its key changes: the
    // view leaves the requested window (the view plus a margin, at most
    // kMaxLabelCells cells) or a version changes. With more than kMaxLabelCells
    // visible cells the gate is closed: nothing is posted. Labels are drawn only
    // from the query's CURRENT result (re-read every frame: a cancelled query has
    // none) whose request belongs to this symbol (serial at or after the symbol
    // epoch) and whose timeframe and tick are both the drawn ones (the node's last
    // frame) and this frame's target; none during a crossfade or a hold (the node
    // may start a transition in the frame being prepared).
    static constexpr uint32_t kMaxLabelCells = 16000;
    static constexpr int kLabelRetryMs = LabelRetryPolicy::kFirstMs, kMaxLabelRetryMs = LabelRetryPolicy::kMaxMs;
    // Render thread (updatePaintNode, after prepareFrame), or GUI thread.
    std::shared_ptr<const LabelCells> labelsForFrame() const;
    // Which columns of `labels` show exactly the picture on screen and this
    // frame's target (matchLabelColumns over the node's last drawn segments, the
    // current SpanSet and live snapshot). Returns the matched count.
    size_t matchLabelColumns(const LabelCells &labels, std::vector<uint8_t> &out) const;
    // A result of this symbol, timeframe and target tick exists but the gate holds
    // it back for now (a crossfade, a hold, the node not yet on the target tick).
    bool labelsPending() const;
    // What the next label layout would draw (0: nothing): the result and its
    // matched columns. The chart compares it after a frame to redraw labels once a
    // transition ends on an otherwise idle chart (S7b review).
    uint64_t labelSignature() const;
    static uint64_t labelSignature(const LabelCells *labels, const std::vector<uint8_t> &columns, size_t matched);
    int labelRetryMs() const { return retry_.delayMs(); }
    const DrawStyle &drawStyle() const { return style_; }
    std::shared_ptr<const HeatmapPalette> palette() const { return palette_; }
    // The liquidity (base-asset quantity, as sensitivityMin/Max) of the valid cells
    // in the last label window of this symbol (5th percentile to maximum): the
    // range slider's ends. valid false until one arrived; kept while the label
    // gate is closed (zoomed out past kMaxLabelCells).
    struct LiquidityRange {
        double lo = 0, hi = 0;
        bool valid = false;
        bool operator==(const LiquidityRange &) const = default;
    };
    LiquidityRange liquidityRange() const { return liquidityRange_; }
    struct LabelCounters {
        uint64_t posted = 0, results = 0, retries = 0, failures = 0;
        uint64_t lastPostedSerial = 0, epoch = 0;
    };
    const LabelCounters &labelCounters() const { return labelCounters_; }
    // Tests: the request posted last (serial 0: none).
    LabelRequest postedLabelRequest() const { return labelsPosted_ ? postedLabels_ : LabelRequest{}; }
    // Tests: Frame::capture for the next frames (nullptr: none).
    void setCaptureForTest(std::shared_ptr<HeatmapCellCapture> capture) { capture_ = std::move(capture); }

signals:
    void snapshotChanged();
    void liveChanged();
    void tickChanged();     // the drawn tick or the mode changed
    void presetsChanged();
    void limitsChanged();   // max/min time or price span changed
    void buildFailed(QString message);
    void labelsChanged();         // a new label result (or a retry): the chart redraws
    void liquidityRangeChanged(); // liquidityRange() changed

private:
    HeatmapDataService *service_ = nullptr;
    int serviceHook_ = 0;
    HeatmapSourceController *controller_ = nullptr;
    std::shared_ptr<HeatmapCapacity> capacity_;
    bool active_ = false;
    std::string symbol_;
    int64_t tfMs_ = 60'000;
    ViewWindow view_;
    bool hasView_ = false; // setView called: views are posted only from then on
    bool priceKnown_ = false;
    double widthPx_ = 0, heightPx_ = 0, dpr_ = 1;
    HeatmapChartSettings settings_;
    PaletteTone tone_;
    std::shared_ptr<const HeatmapPalette> palette_;
    DrawStyle style_;
    std::shared_ptr<const SpanSet> snapshot_;
    std::shared_ptr<const LiveSnapshot> live_;
    std::shared_ptr<const ResolutionSummary> resolution_;
    uint64_t lastLiveVersion_ = 0;
    int64_t lastLiveEndMs_ = 0; // newest open end seen for symbol_ (any timeframe)
    int64_t liveReceivedAtMs_ = 0;
    std::shared_ptr<HeatmapTileStats> tileStats_ = std::make_shared<HeatmapTileStats>();
    bool manualMode_ = false;
    int64_t manualUnits_ = 100;
    ManualTickMemory tickMemory_;
    std::vector<int64_t> offered_;
    // Tick state (written in prepareFrame while the GUI thread is blocked).
    int64_t tickUnits_ = 0, autoUnits_ = 0, postedTickUnits_ = -1;
    bool postedManual_ = false;
    std::optional<int64_t> autoPriceTick_; // fit policy input, not a retained click/window
    struct TickKey {
        uint64_t version = 0;
        const ResolutionSummary *set = nullptr;
        ViewWindow view;
        double heightPx = 0, minRowPx = 0, h = 0;
        int64_t current = 0;
        bool operator==(const TickKey &) const = default;
    } tickKey_;
    uint64_t tickChanges_ = 0;
    uint64_t viewSerial_ = 0, renderedSerial_ = 0;
    double lastMaxTime_ = -1, lastMaxPrice_ = -1, lastMinTime_ = -1, lastMinPrice_ = -1;
    struct ControllerStats {
        HeatmapSourceController::Stats stats;
        bool valid = false;
    };
    std::shared_ptr<ControllerStats> controllerStats_ = std::make_shared<ControllerStats>();
    // Labels (GUI thread, or the render thread while the GUI thread is blocked).
    uint64_t labelSerial_ = 0, labelEpoch_ = 1;
    LabelRequest postedLabels_;
    bool labelsPosted_ = false;
    bool labelResultSeen_ = false;   // the posted request's result arrived (GUI)
    bool labelDropNoticed_ = false;  // ...and a later frame found it dropped (cancel/shed)
    std::shared_ptr<const LabelCells> notifiedLabels_; // the last result that requested a frame
    QTimer *labelRetry_ = nullptr;
    LabelRetryPolicy retry_;
    mutable std::vector<HeatmapTileStats::Segment> segmentScratch_; // reused (matchLabelColumns)
    mutable std::vector<uint8_t> signatureColumns_;                 // reused (labelSignature)
    LiquidityRange liquidityRange_;
    std::vector<double> liquidityScratch_;
    LabelCounters labelCounters_;
    std::shared_ptr<HeatmapCellCapture> capture_;
    std::atomic<int> rowPxDrawn_{0}, colPxDrawn_{0};
    std::atomic<int64_t> proposedTickUnits_{0}; // render thread's choice, committed on the GUI thread
    // The tick policy generation (GUI thread: symbol, timeframe, settings, Manual memory)
    // and the one the pending proposal was chosen under; a mismatch drops the proposal.
    std::atomic<uint64_t> tickPolicy_{0}, proposedPolicy_{0};
    void invalidateTickProposal();
    void commitProposedTick();
    std::atomic<double> rowPxContinuous_{0.0};

    void createController();
    void destroyController();
    void forgetService(); // the service is being destroyed: it deletes the controller itself
    void onSnapshot();
    void onLive();
    void postView();
    void postBudget();
    void postLiveInterval();
    void refreshControllerStats() const;
    void postLabels(const ViewWindow &view);
    void onLabels();
    void onLabelRetry();
    void resetLabels(bool newEpoch);
    int64_t chooseTick(const ViewWindow &view);
    void restoreTick();
    void refreshPresets();
    void refreshStyle();
    void viewChanged();
    void noteLimits();
};
} // namespace heatmap::gpu
