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
    std::atomic<uint32_t> columns{0}, rows{0}, group{0};
    std::atomic<double> tick{0};
};

class LabItem : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
public:
    explicit LabItem(QQuickItem *parent = nullptr);
    ~LabItem() override;
    QString status() const { return status_; }
    Q_INVOKABLE void loadReal(int hours, const QString &layer);
    Q_INVOKABLE void loadSynthetic(int count);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoom(double steps, bool priceOnly, double anchorX, double anchorY);
    Q_INVOKABLE QVariantMap metrics() const;
signals:
    void statusChanged();
protected:
    QQuickRhiItemRenderer *createRenderer() override;
private:
    friend class LabRenderer;
    struct View { double timeLo = 0, timeHi = 1, priceLo = 0, priceHi = 1; } view_;
    std::shared_ptr<const recording::RecordingEntries> data_;
    std::shared_ptr<Telemetry> telemetry_ = std::make_shared<Telemetry>();
    std::shared_ptr<std::atomic<uint64_t>> loadGeneration_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::mutex> loadMutex_ = std::make_shared<std::mutex>();
    QString status_ = QStringLiteral("Select a source");
    uint64_t version_ = 0;
    void accept(std::shared_ptr<const recording::RecordingEntries> data);
};
} // namespace lab
