#pragma once
// Benchmark/inspection harness for the production heatmap GPU path: a plain
// QQuickItem hosting heatmap::gpu::HeatmapRenderNode in the normal scene graph,
// fed by heatmap::SparseColumns composed on a worker thread.
// Slice T (docs/research/2026-09-heatmap-interaction-spec.md, experiments E1-E3):
// a column is exactly the selected timeframe (no auto-timeframe); time zoom-out
// clamps at one column per physical pixel; tick is Auto (minRowPx + hysteresis)
// or Manual (locked preset, remembered per symbol and timeframe, price zoom-out
// clamped at one row per physical pixel, never coarsened).
#include "LabSources.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "render/heatmap/HeatmapRenderNode.hpp"
#include <QElapsedTimer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>

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
public:
    static constexpr double kCrossfadeMs = 150; // experiment E2
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
    Q_INVOKABLE void loadReal(int hours, const QString &layer);
    Q_INVOKABLE void loadSynthetic(int count);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoom(double steps, bool priceOnly, double anchorX, double anchorY);
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
    bool settled() const;
signals:
    void statusChanged();
    void timeframeChanged();
    void tickChanged();
    void presetsChanged();
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
private:
    heatmap::gpu::ViewWindow view_;
    LabSource source_;
    bool loading_ = false;
    std::shared_ptr<heatmap::gpu::HeatmapRenderStats> stats_ = std::make_shared<heatmap::gpu::HeatmapRenderStats>();
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
    bool crossfade_ = false;
    bool persistTickMemory_ = false;
    double initialRowPx_ = 0;
    heatmap::ManualTickMemory tickMemory_;
    QVariantList offeredTicks_;
    QElapsedTimer launched_, lastFrame_;
    double frameMs_ = 0, firstFrameMs_ = 0;
    QMetaObject::Connection frameConnection_;
    int64_t tfMs() const { return int64_t(timeframeMinutes_) * 60'000; }
    std::string symbol() const { return source_.gpu ? source_.gpu->symbol : std::string(); }
    double priceScale() const { return source_.gpu ? source_.gpu->priceScale : 100.0; }
    void accept(LabSource source, bool preserveView, bool final);
    void reload(bool preserveView);
    void clampView();
    void rememberTick();
    void restoreTick();
    double fallbackManualTick() const;
    void loadTickMemory();
};
} // namespace lab
