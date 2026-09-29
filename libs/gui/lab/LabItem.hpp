#pragma once
// Benchmark/inspection harness for the production heatmap GPU path: a plain
// QQuickItem hosting heatmap::gpu::HeatmapRenderNode in the normal scene graph,
// fed by heatmap::SparseColumns composed on a worker thread.
#include "LabSources.hpp"
#include "render/heatmap/HeatmapRenderNode.hpp"
#include <QElapsedTimer>
#include <QQuickItem>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>

namespace lab {
class LabItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int timeframeMinutes READ timeframeMinutes WRITE setTimeframeMinutes NOTIFY timeframeChanged)
    Q_PROPERTY(bool autoTimeframe READ autoTimeframe WRITE setAutoTimeframe NOTIFY timeframeChanged)
    Q_PROPERTY(double minColumnPx READ minColumnPx WRITE setMinColumnPx NOTIFY timeframeChanged)
    Q_PROPERTY(double manualTick READ manualTick WRITE setManualTick NOTIFY tickChanged)
public:
    explicit LabItem(QQuickItem *parent = nullptr);
    ~LabItem() override;
    QString status() const { return status_; }
    int timeframeMinutes() const { return timeframeMinutes_; }
    bool autoTimeframe() const { return autoTimeframe_; }
    double minColumnPx() const { return minColumnPx_; }
    double manualTick() const { return manualTick_; }
    void setTimeframeMinutes(int minutes);
    void setAutoTimeframe(bool enabled);
    void setMinColumnPx(double pixels);
    void setManualTick(double tick);
    Q_INVOKABLE void loadReal(int hours, const QString &layer);
    Q_INVOKABLE void loadSynthetic(int count);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoom(double steps, bool priceOnly, double anchorX, double anchorY);
    Q_INVOKABLE QVariantMap metrics() const;
    Q_INVOKABLE bool saveScreenshot(const QString &path = {});
    // True once the newest requested source (all load phases) is uploaded and drawn.
    bool settled() const;
signals:
    void statusChanged();
    void timeframeChanged();
    void tickChanged();
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
    bool autoTimeframe_ = false;
    double minColumnPx_ = 1.0;
    double manualTick_ = 0;
    QElapsedTimer launched_, lastFrame_;
    double frameMs_ = 0, firstFrameMs_ = 0;
    QMetaObject::Connection frameConnection_;
    int64_t tfMs() const { return int64_t(timeframeMinutes_) * 60'000; }
    void accept(LabSource source, bool preserveView, bool final);
    void reload(bool preserveView);
    void clampTimeSpan();
};
} // namespace lab
