// GUI-owned selection/subscription; all chunk composition runs on flow-chunks.
// The service joins before m_client dies. No worker captures chart QObjects or
// reads GUI state: inputs are copied and publications are generation-fenced.
#include "RemoteGridDataSource.hpp"
#include "config/AgentHostMode.hpp"
#include "SentinelLogging.hpp"
#include <QDateTime>
namespace f = sentinel::flow;
namespace w = protocol::chunks;
struct RemoteGridDataSource::FlowWork {
    f::FlowChunkView rows, summary;
    quint64 wantGeneration = 0;
};
namespace {
bool flowPermitted(const QString& symbol) {
    if (AgentHostMode::symbolAllowed(symbol)) return true;
    sLog_Warning("agent-host: flow refused: symbol=" << symbol);
    return false;
}
f::chunks::Status status(SentinelStreamClient::FlowStatus value) {
    using S = SentinelStreamClient::FlowStatus;
    return value == S::Complete || value == S::Live ? f::chunks::Status::Complete :
        value == S::Unrecorded ? f::chunks::Status::Unrecorded :
        value == S::Building ? f::chunks::Status::Building : f::chunks::Status::Partial;
}
}
RemoteGridDataSource::~RemoteGridDataSource() {
    m_flowTimer.stop(); m_flowViewTimer.stop();
    m_flowChunks.reset();
}
void RemoteGridDataSource::initializeFlow() {
    m_flowClock.start();
    m_flowChunks = std::make_unique<f::FlowChunkClient>([this](QObject* parent) {
        return new w::ClientTransport(m_client, parent);
    });
    m_flowTimer.setSingleShot(true); m_flowTimer.setTimerType(Qt::PreciseTimer);
    m_flowViewTimer.setSingleShot(true); m_flowViewTimer.setInterval(0);
    connect(&m_flowTimer, &QTimer::timeout, this, &RemoteGridDataSource::startFlowLive);
    connect(&m_flowViewTimer, &QTimer::timeout, this, &RemoteGridDataSource::refreshFlowView);
    connect(&m_client, &SentinelStreamClient::flowBarsReceived, this,
            &RemoteGridDataSource::onFlowBarsReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::flowLiveFailed, this, [this](quint64 sub, const QString& message) {
        if (!sub || sub != m_flowSubscription) return;
        m_flowSubscription = 0; m_flowReplayReady = false;
        sLog_Warning("Flow live refused symbol=" << m_flowSymbol << " retryMs=" << m_flowRetryMs << " reason=" << message);
        m_flowTimer.start(m_flowRetryMs); m_flowRetryMs = std::min(5000, m_flowRetryMs * 2);
    }, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::flowLiveDropped, this,
        [this](quint64 sub, const QString& symbol, int64_t tf, int64_t) {
            if (sub != m_flowSubscription || symbol != m_flowSymbol || tf != m_flowTimeframeMs) return;
            // A replay may have lost a whole bucket. Restart its complete replay,
            // with the same bounded retry policy; no legacy history repair path.
            m_client.unsubscribeFlowLive(sub);
            m_flowSubscription = 0; m_flowReplayReady = false;
            m_flowTimer.start(m_flowRetryMs);
            m_flowRetryMs = std::min(5000, m_flowRetryMs * 2);
        }, Qt::QueuedConnection);
    auto& fetch = m_flowChunks->fetcher();
    connect(&fetch, &f::FlowChunkFetcher::availabilityChanged, this, [this](w::Availability a) {
        if (a.instrument != "coinbase:" + m_flowSymbol.toStdString()) return;
        m_flowAvailabilityReady = true;
        const auto work = m_flowWork;
        if (work) m_flowChunks->queued([work, from = a.liveFromMs](auto&) {
            work->rows.retention(from); work->summary.retention(from);
        });
        startFlowLive(); m_flowViewTimer.start();
    }, Qt::QueuedConnection);
    const auto changed = [this](const w::Key& key) {
        if (key.instrument == "coinbase:" + m_flowSymbol.toStdString()) m_flowViewTimer.start();
    };
    connect(&fetch, &f::FlowChunkFetcher::chunkStored, this, [this, changed](const w::Key& key) {
        if (key.instrument == "coinbase:" + m_flowSymbol.toStdString()) m_flowProbe.fetched();
        changed(key);
    }, Qt::QueuedConnection);
    connect(&fetch, &f::FlowChunkFetcher::chunkHidden, this, changed, Qt::QueuedConnection);
    connect(&fetch, &f::FlowChunkFetcher::chunkRevised, this, changed, Qt::QueuedConnection);
    connect(&fetch, &f::FlowChunkFetcher::storeCleared, this, [this] {
        m_flowBuffer.clear(); m_flowSummary.clear(); m_flowViewTimer.start();
    }, Qt::QueuedConnection);
}
void RemoteGridDataSource::subscribeFlowLive(const QString& symbol, int64_t tf) {
    if (symbol == m_flowSymbol && tf == m_flowTimeframeMs) return;
    unsubscribeFlowLive();
    if (!m_connectionActive || !m_flowConfigReady || symbol.isEmpty() ||
        (tf != 1000 && (tf < 60000 || tf > 86400000 || tf % 60000 || 86400000 % tf)) || !flowPermitted(symbol)) return;
    m_flowSymbol = symbol; m_flowTimeframeMs = tf; m_flowRetryMs = 100;
    auto& held = m_flowOwnership[{symbol, tf}];
    if (!held) held = std::make_shared<FlowWork>();
    m_flowWork = held;
    // One-second flow stays RAM-only and needs no sealed availability.
    if (tf == 1000) startFlowLive();
}
void RemoteGridDataSource::startFlowLive() {
    if (!m_connectionActive || !m_flowConfigReady || m_flowSymbol.isEmpty() || m_flowSubscription ||
        (m_flowTimeframeMs != 1000 && !m_flowAvailabilityReady) || m_flowTimer.isActive() || !flowPermitted(m_flowSymbol)) return;
    const auto work = m_flowWork;
    m_flowChunks->queued([work](auto&) { work->rows.resetLive(); work->summary.resetLive(); });
    m_flowReplayReady = m_flowTimeframeMs == 1000;
    m_flowSubscription = m_client.subscribeFlowLive(m_flowSymbol.toStdString(), m_flowTimeframeMs);
    m_flowViewTimer.start();
}
void RemoteGridDataSource::unsubscribeFlowLive() {
    if (m_flowSubscription) m_client.unsubscribeFlowLive(m_flowSubscription);
    ++m_flowGeneration;
    m_flowSubscription = 0; m_flowSymbol.clear(); m_flowTimeframeMs = 0;
    m_flowFrom = m_flowTo = 0; m_flowPan = 0; m_flowAnchor = -1;
    m_flowAvailabilityReady = m_flowReplayReady = false;
    m_flowTimer.stop(); m_flowViewTimer.stop();
    m_flowBuffer.clear(); m_flowSummary.clear(); m_flowProbe.reset();
    const auto work = std::exchange(m_flowWork, {});
    m_flowChunks->queued([work](auto& fetch) {
        if (work) { work->rows.resetLive(); work->summary.resetLive(); }
        fetch.release(1); fetch.release(2);
    });
}
void RemoteGridDataSource::setFlowViewport(const QString& symbol, int64_t tf, int64_t from, int64_t to,
                                          int64_t anchor, bool rows) {
    if (symbol != m_flowSymbol || tf != m_flowTimeframeMs || from < 0 || to <= from || !flowPermitted(symbol)) return;
    if (from == m_flowFrom && to == m_flowTo && anchor == m_flowAnchor && rows == m_flowRows) return;
    if (m_flowTo > m_flowFrom) {
        const auto moved = (from - m_flowFrom) + (to - m_flowTo);
        if (moved) m_flowPan = moved > 0 ? 1 : -1;
    }
    ++m_flowGeneration;
    m_flowFrom = from; m_flowTo = to; m_flowAnchor = anchor; m_flowRows = rows;
    if (m_flowProbe.on()) m_flowProbe.viewChanged(symbol, tf, from, to, m_flowClock.elapsed());
    m_flowViewTimer.start();
}
void RemoteGridDataSource::refreshFlowView() {
    if (!m_flowWork || m_flowTo <= m_flowFrom || !flowPermitted(m_flowSymbol)) return;
    if (m_flowComposing) { m_flowDirty = true; return; }
    m_flowComposing = true; m_flowDirty = false;
    const auto work = m_flowWork;
    const auto generation = m_flowGeneration;
    const auto symbol = m_flowSymbol;
    const auto anchor = m_flowAnchor;
    const bool rows = m_flowRows;
    f::FlowChunkWant want{"coinbase:" + symbol.toStdString(), rows ? "flow.rows" : "flow.summary",
                         m_flowTimeframeMs, m_flowFrom, m_flowTo, m_flowPan};
    auto* cache = &m_flowChunks->cache();
    m_flowChunks->queued([this, cache, work, generation, symbol, anchor, rows, want](auto& fetch) mutable {
        if (want.timeframeMs >= 60000 && work->wantGeneration != generation) {
            work->wantGeneration = generation;
            fetch.want(1, want);
            if (anchor >= 0 && anchor < want.fromMs) {
                auto prefix = want; prefix.channel = "flow.summary"; prefix.fromMs = anchor; prefix.toMs = want.fromMs;
                fetch.want(2, prefix);
            } else fetch.release(2);
        }
        const auto heldAvailability = fetch.availability(want.instrument);
        auto availability = heldAvailability.value_or(w::Availability{});
        availability.instrument = want.instrument;
        work->rows.retention(availability.liveFromMs); work->summary.retention(availability.liveFromMs);
        auto picture = rows ? work->rows.compose(*cache, want, availability) : f::FlowViewSnapshot{};
        want.channel = "flow.summary";
        auto summary = work->summary.compose(*cache, want, availability, anchor);
        QMetaObject::invokeMethod(this, [this, generation, symbol, tf = want.timeframeMs,
                                        available = heldAvailability.has_value(), picture = std::move(picture), summary = std::move(summary)]() mutable {
            m_flowComposing = false;
            if (m_flowDirty) m_flowViewTimer.start();
            if (generation != m_flowGeneration) return;
            if (available) { m_flowAvailabilityReady = true; startFlowLive(); }
            m_flowBuffer.replace(symbol, tf, std::move(picture));
            m_flowSummary.replace(symbol, tf, std::move(summary));
            if (m_flowProbe.on())
                m_flowProbe.applied(m_flowRows ? m_flowBuffer : m_flowSummary, m_flowClock.elapsed(), QDateTime::currentMSecsSinceEpoch());
            auto notice = std::make_shared<SentinelStreamClient::FlowBatch>();
            notice->symbol = symbol; notice->timeframeMs = tf;
            emit flowBarsReceived(std::move(notice));
        }, Qt::QueuedConnection);
    });
}
void RemoteGridDataSource::onFlowBarsReceived(SentinelStreamClient::FlowBatchPtr batch) {
    if (!batch || !m_flowWork || !m_flowSubscription || batch->id != m_flowSubscription) return;
    const auto work = m_flowWork;
    if (batch->replayReady) {
        m_flowReplayReady = true; m_flowRetryMs = 100;
        m_flowChunks->queued([work, from = batch->liveFromMs](auto&) {
            work->rows.retention(from); work->summary.retention(from);
        });
    } else {
        if (batch->symbol != m_flowSymbol || batch->timeframeMs != m_flowTimeframeMs) return;
        m_flowChunks->queued([work, batch](auto&) {
            for (const auto& record : batch->bars) {
                auto coverage = record.complete ? f::chunks::Status::Complete : f::chunks::Status::Partial;
                for (const auto& range : batch->ranges)
                    if (range.fromMs <= record.bar.startMs && record.bar.startMs < range.toMs) coverage = status(range.status);
                f::FlowViewBar live{record.bar, record.revision, record.complete, false};
                work->rows.live(live, coverage);
                decltype(live.bar.rows){}.swap(live.bar.rows);
                decltype(live.bar.notable){}.swap(live.bar.notable); live.bar.maxRowVol = 0;
                work->summary.live(std::move(live), coverage);
            }
        });
    }
    m_flowViewTimer.start();
}
