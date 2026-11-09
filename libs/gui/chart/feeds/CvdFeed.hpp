#pragma once
#include "chart/core/CvdSeries.hpp"
#include "chart/feeds/FlowBufferAccess.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
#include <QTimer>
#include <memory>
#include <mutex>
namespace chart {
class ChartViewController;
class PublishedFrame;
class LayerVisibility;
class ChartTpoQml;
struct CvdPresentation {
    int stripPx = 0;
    CvdReset reset = CvdReset::Visible;
    bool area = false;
};
struct CvdSnapshot {
    CvdSeries series;
    int64_t timeframeMs = 0;
    CvdReset reset = CvdReset::Visible;
    std::array<char, 80> label{};
};
class CvdMailbox {
public:
    CvdMailbox();
    CvdSnapshot& back() { return *back_; }
    void post();
    bool take(std::unique_ptr<CvdSnapshot>&);
private:
    std::mutex mutex_;
    std::unique_ptr<CvdSnapshot> back_, pending_;
    bool fresh_ = false;
};
struct CvdFeedPorts {
    std::function<MappingFrameContext()> frame;
    std::function<QString()> symbol;
    std::function<int()> sessionType;
    std::function<void()> update;
};
class CvdFeed final : public QObject {
public:
    CvdFeed(CvdMailbox&, CvdPresentation&);
    void bind(ChartViewController&, const PublishedFrame&, LayerVisibility&, ChartTpoQml&);
    void bindSource(IGridDataSource&);
    void setPorts(CvdFeedPorts ports) { ports_ = std::move(ports); }
    void onFrameChanged();
    void receive(SentinelStreamClient::FlowBatchPtr);
    void publish();
    int64_t historyAnchor(int64_t start, int64_t timeframe) const;
    static void build(CvdSnapshot&, const FlowSeriesBuffer::Series&, const MappingFrameContext&, CvdReset, int sessionType);
private:
    void selectSymbol();
    CvdMailbox& mailbox_;
    CvdPresentation& presentation_;
    CvdFeedPorts ports_;
    FlowBufferAccess buffer_;
    QTimer timer_;
    QMetaObject::Connection sourceConnection_;
    QString symbol_;
};
} // namespace chart
