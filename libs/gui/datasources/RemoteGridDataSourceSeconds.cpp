// Bounded one-second fallback until the server's live replay covers RAWL2.
// The transport decodes off-thread; this GUI adapter merges at most one page.
#include "RemoteGridDataSource.hpp"
#include "config/AgentHostMode.hpp"
#include "SentinelLogging.hpp"

void RemoteGridDataSource::requestNextFlowSecondPage() {
    if (!m_connectionActive || !m_flowConfigReady || m_flowTimeframeMs != 1000 ||
        m_flowTo <= m_flowFrom || !AgentHostMode::symbolAllowed(m_flowSymbol)) return;
    const auto now = candleNow();
    const auto view = sentinel::flow::paddedFlowView({{}, {}, 1000, m_flowFrom, m_flowTo});
    m_flowSeconds.window(view.fromMs, view.toMs, m_flowAnchor);
    m_flowBuffer.retain(1000, m_flowSeconds.from(), m_flowSeconds.to());
    if (const auto request = m_flowSeconds.next(now))
        m_client.requestFlowSeconds(request->id, m_flowSymbol.toStdString(), request->toMs-1000,
                                    int((request->toMs-request->fromMs)/1000));
    const auto wake = m_flowSeconds.wakeAt(now);
    if (wake != INT64_MAX) m_flowSecondsTimer.start(int(std::clamp<int64_t>(wake-now, 1, INT_MAX)));
    else m_flowSecondsTimer.stop();
    publishFlowSeconds();
}
void RemoteGridDataSource::onFlowSeconds(SentinelStreamClient::FlowBatchPtr batch) {
    if (m_flowTimeframeMs != 1000 || batch->timeframeMs != 1000 || batch->symbol != m_flowSymbol ||
        !m_flowSeconds.matches(batch->id)) return;
    if (batch->status == SentinelStreamClient::FlowStatus::Refused) {
        failFlowSeconds(batch->id, batch->error); return;
    }
    const auto request = *m_flowSeconds.flight();
    const bool outside = batch->oldestMs != request.fromMs ||
        std::any_of(batch->bars.begin(), batch->bars.end(), [&](const auto& r) {
            return r.bar.startMs < request.fromMs || r.bar.startMs >= request.toMs;
        }) || std::any_of(batch->ranges.begin(), batch->ranges.end(), [&](const auto& r) {
            return r.fromMs < request.fromMs || r.toMs > request.toMs;
        });
    if (outside) { failFlowSeconds(batch->id, "invalid second history window"); return; }
    m_flowBuffer.apply(*batch, candleNow(), m_flowSeconds.from(), m_flowSeconds.to());
    m_flowBuffer.retain(1000, m_flowSeconds.from(), m_flowSeconds.to());
    m_flowSeconds.accepted(candleNow(), batch->status == SentinelStreamClient::FlowStatus::Building);
    m_flowProbe.fetched();
    requestNextFlowSecondPage();
}
void RemoteGridDataSource::failFlowSeconds(quint64 id, const QString& message) {
    if (m_flowTimeframeMs != 1000 || !m_flowSeconds.matches(id)) return;
    sLog_Warning("Flow second history failed symbol=" << m_flowSymbol << " req=" << id << " reason=" << message);
    const bool oversized = message.contains("15 MiB") || message.contains("too large");
    const bool fatal = message.contains("invalid") || message.contains("unsupported");
    m_flowSeconds.failed(candleNow(), oversized, fatal);
    requestNextFlowSecondPage();
}
void RemoteGridDataSource::publishFlowSeconds() {
    if (m_flowProbe.on()) m_flowProbe.applied(m_flowBuffer, m_flowClock.elapsed(), candleNow());
    auto notice = std::make_shared<SentinelStreamClient::FlowBatch>();
    notice->symbol = m_flowSymbol; notice->timeframeMs = 1000;
    emit flowBarsReceived(std::move(notice));
}
