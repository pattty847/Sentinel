#pragma once
#include "AgentApiCodec.hpp"
#include "chart/core/ViewCommands.hpp"
#include <functional>
#include <utility>
namespace chart { class ChartViewController; class PublishedFrame; class FrameAck; }

class IGridDataSource;
namespace chart { class FlowFeed; class LayerVisibility; }
class ChartAgentPort {
public:
    ChartAgentPort() = default;
    ChartAgentPort(chart::ChartViewController& view, const chart::PublishedFrame& published,
                   chart::FrameAck& ack, std::function<void()> update,
                   chart::FlowFeed* flow = nullptr, chart::LayerVisibility* layers = nullptr)
        : view_(&view), published_(&published), ack_(&ack), update_(std::move(update)), flow_(flow), layers_(layers) {}
    void bindFlowSource(IGridDataSource&) const;
    QJsonObject footprintSettings() const;
    static QJsonObject footprintPatch(const QJsonObject&);
    void applyFootprintSettings(const QJsonObject&) const;
    AgentApi::ViewportSnapshot snapshot(AgentApi::Metadata meta = {}) const;
    AgentApi::ControlApply applyViewport(const AgentApi::ControlBody& body) const;
    QString applyViewportRequest(const chart::ViewportRequest& request) const;
    std::pair<quint64, quint64> rendered() const;
    void setControlRevision(quint64 revision, quint64 selectionEpoch) const;
private:
    chart::ChartViewController* view_ = nullptr;
    const chart::PublishedFrame* published_ = nullptr;
    chart::FrameAck* ack_ = nullptr;
    std::function<void()> update_;
    chart::FlowFeed* flow_ = nullptr;
    chart::LayerVisibility* layers_ = nullptr;
};
