#pragma once
#include <QObject>
#include <functional>
#include <memory>

class DataProcessor;
class QThread;

namespace chart {
struct TradeLayers;
class TradeOverlayFeeds : public QObject {
public:
    explicit TradeOverlayFeeds(TradeLayers& layers, QObject* context, std::function<void()> update);
    ~TradeOverlayFeeds() override;
    DataProcessor* processor() const { return processor_.get(); }
    void connectDataProcessorSignals();
    void startThread();
    void startProcessing();
    void clearData();
    void shutdown();
private:
    TradeLayers& layers_;
    QObject* context_;
    std::function<void()> update_;
    std::unique_ptr<DataProcessor> processor_;
    std::unique_ptr<QThread> thread_;
};
} // namespace chart
