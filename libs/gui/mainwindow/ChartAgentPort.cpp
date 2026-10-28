#include "ChartAgentPort.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/core/FrameAck.hpp"
#include <cmath>

AgentApi::ViewportSnapshot ChartAgentPort::snapshot(AgentApi::Metadata meta) const {
    AgentApi::ViewportSnapshot s;
    s.meta = std::move(meta);
    if (!view_) return s;
    const auto view = view_->snapshot();
    const int64_t tf = view.timeframeMs;
    if (tf > 0) {
        s.heatmapTimeframeMs = tf;
        s.candleTimeframeMs = tf; // v1 QML links candle cadence to renderer cadence.
    }
    s.followLive = view.followLive;
    s.autoScale = view.autoScale;
    s.lockMid = view.lockMid;
    if (!view.timeWindowValid) return s;
    const qint64 start = view.start;
    const qint64 end = view.end;
    const double low = view.low;
    const double high = view.high;
    if (end > start) { s.startMs = start; s.endMs = end; }
    if (std::isfinite(low) && std::isfinite(high) && high > low) { s.priceMin = low; s.priceMax = high; }
    s.viewportVersion = view.viewportVersion;
    if (view.width > 0) s.widthPx = view.width;
    if (view.height > 0) s.heightPx = view.height;
    // What the last frame drew (the raster camera; the bounds above are the stored view).
    if (const auto cam = published_->current().raster; cam.valid)
        s.drawn = AgentApi::ViewportSnapshot::Drawn{cam.drawnStartMs, cam.drawnEndMs, cam.drawnMinPrice,
                                                    cam.drawnMaxPrice, cam.rowPx,    cam.colPx, cam.dpr};
    return s;
}

AgentApi::ControlApply ChartAgentPort::applyViewport(const AgentApi::ControlBody& body) const {
    AgentApi::ControlApply out;
    const auto current = snapshot();
    if (!current.startMs || !current.endMs || !current.priceMin || !current.priceMax) {
        out.status = 503; out.code = "viewport_unavailable"; out.message = "Chart viewport is not ready";
        return out;
    }
    const QString error = applyViewportRequest(
        {body.startMs, body.endMs, body.priceMin, body.priceMax, body.followLive, body.autoScale, body.lockMid, body.fit});
    if (error == "fit_unavailable") {
        out.status = 409; out.code = "fit_unavailable";
        out.message = "Auto-fit needs known data (a live anchor or a price)";
        return out;
    }
    if (!body.fit.isEmpty()) out.data["fit"] = body.fit;
    const auto after = snapshot();
    out.viewportVersion = after.viewportVersion.value_or(0);
    out.data["viewportVersion"] = QString::number(out.viewportVersion);
    out.data["followLive"] = after.followLive.value_or(false);
    out.data["autoScale"] = after.autoScale.value_or(false);
    out.data["lockMid"] = after.lockMid.value_or(false);
    return out;
}

QString ChartAgentPort::applyViewportRequest(const chart::ViewportRequest& request) const {
    return view_ ? view_->applyViewportRequest(request) : QStringLiteral("viewport_unavailable");
}

std::pair<quint64, quint64> ChartAgentPort::rendered() const {
    return {ack_ ? ack_->renderedControlRevision() : 0, ack_ ? ack_->renderedFrameId() : 0};
}

void ChartAgentPort::setControlRevision(quint64 revision, quint64 selectionEpoch) const {
    if (!ack_) return;
    ack_->setAgentControlRevision(revision, selectionEpoch, snapshot().viewportVersion.value_or(0));
    update_();
}
