#pragma once
#include "GpuBinner.hpp"
#include <QQuickRhiItem>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>

namespace lab {
struct Telemetry {
    std::atomic<double> frameMs{0}, binSubmitMs{0}, gpuFrameMs{0}, firstFrameMs{0};
    std::atomic<uint64_t> gpuBytes{0}, frames{0};
    std::atomic<uint64_t> paintedVersion{0};
    std::atomic<uint64_t> rebins{0};
    std::atomic<uint32_t> columns{0}, rows{0}, group{0};
    std::atomic<double> tick{0};
};

class LabItem : public QQuickRhiItem {
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
signals:
    void statusChanged();
    void timeframeChanged();
    void tickChanged();
protected:
    QQuickRhiItemRenderer *createRenderer() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
private:
    friend class LabRenderer;
    struct View { double timeLo = 0, timeHi = 1, priceLo = 0, priceHi = 1; } view_;
    std::shared_ptr<const recording::RecordingEntries> data_;
    std::shared_ptr<Telemetry> telemetry_ = std::make_shared<Telemetry>();
    std::shared_ptr<std::atomic<uint64_t>> loadGeneration_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::mutex> loadMutex_ = std::make_shared<std::mutex>();
    QString status_ = QStringLiteral("Select a source");
    int realHours_ = 24;
    QString realLayer_ = QStringLiteral("near");
    bool realMode_ = false;
    uint64_t version_ = 0;
    int timeframeMinutes_ = 1;
    bool autoTimeframe_ = false;
    double minColumnPx_ = 1.0;
    double manualTick_ = 0;
    void accept(std::shared_ptr<const recording::RecordingEntries> data, bool preserveView = false);
    void loadRealInternal(int hours, const QString &layer, bool preserveView);
};
} // namespace lab
