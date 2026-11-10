#pragma once
#include "chart/core/CvdSeries.hpp"
#include "chart/core/FlowFooter.hpp"
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
    bool flowFooter = false;
    CvdReset reset = CvdReset::Visible;
    bool area = false;
    // GUI mailbox publication, read only during render sync (GUI is blocked).
    int64_t dataTimeframeMs = 0;
    CvdReset dataReset = CvdReset::Visible;
    bool hasData = false;
    bool matches(int64_t timeframeMs) const {
        return hasData && dataReset == reset &&
            dataTimeframeMs == std::max<int64_t>(1000, timeframeMs);
    }
    bool drawable(int64_t timeframeMs) const { return stripPx > 0 && matches(timeframeMs); }
    double bottomInset(int64_t timeframeMs, double surfaceHeight) const {
        if (!matches(timeframeMs)) return 0;
        const double strip = std::clamp(double(stripPx), 0.0, std::max(0.0, surfaceHeight));
        const double footer = flowFooter && surfaceHeight - strip >= FlowFooterLayout::height ? FlowFooterLayout::height : 0;
        return strip + footer;
    }
};
struct CvdSnapshot {
    CvdSeries series;
    std::array<FlowFooterTotals, CvdSeries::kCapacity> totals{};
    int64_t timeframeMs = 0;
    CvdReset reset = CvdReset::Visible;
    std::array<char, 80> label{};
};
class CvdMailbox {
public:
    explicit CvdMailbox(CvdPresentation* presentation = nullptr);
    CvdSnapshot& back() { return *back_; }
    void post();
    bool take(std::unique_ptr<CvdSnapshot>&);
private:
    CvdPresentation* presentation_ = nullptr;
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
