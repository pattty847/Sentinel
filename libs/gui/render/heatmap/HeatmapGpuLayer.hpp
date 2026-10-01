#pragma once
// The main chart's GPU heatmap layer (S6b, docs/research/2026-10-s6-plan.md
// section 2): LabItem without view ownership. UnifiedGridRenderer owns one per
// chart (GUI thread) and gives it the symbol, timeframe, the committed view of
// its GridViewState and the chart settings; the layer owns:
// - a controller handle from the process HeatmapDataService (one per chart),
// - the latest SpanSet, LiveSnapshot and ResolutionSummary,
// - the tick policy (interaction spec rules 1-4) with its TickKey cache,
//   Manual tick memory per (symbol, timeframe) and the offered presets,
// - the per-chart node frame inputs (palette, style, budgets) and HeatmapTileStats.
// prepareFrame() runs in updatePaintNode (render thread, GUI thread blocked): it
// takes the freshest snapshot, chooses the tick (cached by TickKey: no work when
// nothing changed) and fills the HeatmapTileNode frame in the same frame, then
// queues the tick request to the controller. No allocation per frame beyond the
// lab's (the node frame holds shared pointers).
#include "HeatmapDataService.hpp"
#include "HeatmapPalette.hpp"
#include "HeatmapTileNode.hpp"
#include "heatmap/HeatmapChartSettings.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <QJsonObject>
#include <QObject>
#include <memory>
#include <optional>
#include <vector>

namespace heatmap::gpu {
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

    // Render thread, GUI thread blocked (updatePaintNode). `view` is the view the
    // chart draws now (drag offset baked in). Returns false when there is nothing
    // to draw yet (no controller).
    bool prepareFrame(HeatmapTileNode::Frame &frame, const QRectF &rect, const ViewWindow &view);

    // Spec rules 1, 2 and 9 for the chart's GridViewState (0 = none): one column
    // per physical pixel; Manual: one row per physical pixel.
    double maxTimeSpanMs() const;
    double maxPriceSpan() const;
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
    // Agent API heatmap state (plan section 5); refreshes the controller stats
    // asynchronously for the next call.
    QJsonObject state() const;

signals:
    void snapshotChanged();
    void liveChanged();
    void tickChanged();     // the drawn tick or the mode changed
    void presetsChanged();
    void limitsChanged();   // maxTimeSpanMs / maxPriceSpan changed
    void buildFailed(QString message);

private:
    HeatmapDataService *service_ = nullptr;
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
    int64_t liveReceivedAtMs_ = 0;
    std::shared_ptr<HeatmapTileStats> tileStats_ = std::make_shared<HeatmapTileStats>();
    bool manualMode_ = false;
    int64_t manualUnits_ = 100;
    ManualTickMemory tickMemory_;
    std::vector<int64_t> offered_;
    // Tick state (written in prepareFrame while the GUI thread is blocked).
    int64_t tickUnits_ = 0, autoUnits_ = 0, postedTickUnits_ = -1;
    bool postedManual_ = false;
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
    double lastMaxTime_ = -1, lastMaxPrice_ = -1;
    struct ControllerStats {
        HeatmapSourceController::Stats stats;
        bool valid = false;
    };
    std::shared_ptr<ControllerStats> controllerStats_ = std::make_shared<ControllerStats>();

    void createController();
    void destroyController();
    void onSnapshot();
    void onLive();
    void postView();
    void postBudget();
    int64_t chooseTick(const ViewWindow &view);
    void restoreTick();
    void refreshPresets();
    void refreshStyle();
    void viewChanged();
    void noteLimits();
};
} // namespace heatmap::gpu
