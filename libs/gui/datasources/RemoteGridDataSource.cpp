#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <QDateTime>
#include "../config/AgentHostMode.hpp"
#include "../config/GuiConfigStore.hpp"

namespace {
std::pair<double, double> computeBandRange(const std::vector<OrderBookLevel>& bids,
                                           const std::vector<OrderBookLevel>& asks,
                                           double bandPct) {
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    bool hasPrice = false;

    for (const auto& level : bids) {
        if (level.price > bestBid) {
            bestBid = level.price;
        }
        if (!hasPrice) {
            minPrice = level.price;
            maxPrice = level.price;
            hasPrice = true;
        } else {
            minPrice = std::min(minPrice, level.price);
            maxPrice = std::max(maxPrice, level.price);
        }
    }

    for (const auto& level : asks) {
        if (bestAsk <= 0.0 || level.price < bestAsk) {
            bestAsk = level.price;
        }
        if (!hasPrice) {
            minPrice = level.price;
            maxPrice = level.price;
            hasPrice = true;
        } else {
            minPrice = std::min(minPrice, level.price);
            maxPrice = std::max(maxPrice, level.price);
        }
    }

    if (bestBid > 0.0 && bestAsk > 0.0) {
        const double mid = (bestBid + bestAsk) * 0.5;
        const double halfRange = mid * bandPct;
        return {mid - halfRange, mid + halfRange};
    }

    if (hasPrice && maxPrice > minPrice) {
        const double pad = std::max(1e-6, (maxPrice - minPrice) * 0.10);
        return {std::max(0.0, minPrice - pad), maxPrice + pad};
    }

    return {0.0, 1.0};
}
}

RemoteGridDataSource::RemoteGridDataSource(const QString& host, const QString& port,
                                           const QString& caFile, QObject* parent)
    : IGridDataSource(parent)
    , m_client(host.toStdString(), port.toStdString(), caFile.toStdString())
{
    qRegisterMetaType<HeatmapHistoryColumn>("HeatmapHistoryColumn");
    qRegisterMetaType<QVector<HeatmapHistoryColumn>>("QVector<HeatmapHistoryColumn>");
    qRegisterMetaType<HeatmapSlice>("HeatmapSlice");
    qRegisterMetaType<FootprintSlice>("FootprintSlice");
    qRegisterMetaType<TpoSlice>("TpoSlice");
    qRegisterMetaType<BookDelta>("BookDelta");
    qRegisterMetaType<std::vector<BookDelta>>("BookDeltaVector");
    qRegisterMetaType<trading::OrderUpdate>("trading::OrderUpdate");
    qRegisterMetaType<trading::PositionUpdate>("trading::PositionUpdate");
    qRegisterMetaType<trading::RiskOrderUpdate>("trading::RiskOrderUpdate");
    qRegisterMetaType<trading::AlgoOrderEvent>("trading::AlgoOrderEvent");
    qRegisterMetaType<trading::PnlSnapshot>("trading::PnlSnapshot");
    m_candleBuffer = std::make_unique<CandleSeriesBuffer>(this);
    m_candleBackfillTimer.setSingleShot(true);
    m_candleBackfillTimer.setInterval(100);
    connect(&m_candleBackfillTimer, &QTimer::timeout, this, &RemoteGridDataSource::requestNextCandlePage);
    connect(&m_client, &SentinelStreamClient::candleHistoryFailed, this,
            [this](const QString& symbol) {
                if (m_candleBackfill.fail(symbol, QDateTime::currentMSecsSinceEpoch()))
                    requestNextCandlePage();
            }, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::tradeReceived,
            this, &IGridDataSource::tradeReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::snapshotReceived,
            this, &RemoteGridDataSource::onSnapshotReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::l2UpdateReceived,
            this, &RemoteGridDataSource::onL2UpdateReceived, Qt::QueuedConnection);
    m_bookSnapshotTimer.setInterval(250);
    connect(&m_bookSnapshotTimer, &QTimer::timeout, this, [this] {
        processBookSnapshotDeadlines(QDateTime::currentMSecsSinceEpoch());
    });
    connect(&m_client, &SentinelStreamClient::heatmapSliceReceived,
            this, &RemoteGridDataSource::onHeatmapSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::footprintSliceReceived,
            this, &RemoteGridDataSource::onFootprintSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::tpoSliceReceived,
            this, &RemoteGridDataSource::onTpoSliceReceived, Qt::QueuedConnection);
    // Queued after the chunk's slices, so pacing never outruns their delivery.
    connect(&m_client, &SentinelStreamClient::tpoHistoryChunkReceived,
            this, &IGridDataSource::tpoHistoryChunkReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::tpoHistoryFailed,
            this, &IGridDataSource::tpoHistoryFailed, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::volumeProfileSliceReceived,
            this, &RemoteGridDataSource::onVolumeProfileSliceReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::heatmapHistoryReceived,
            this, &RemoteGridDataSource::onHeatmapHistoryReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::recordingViewError,
            this, &IGridDataSource::recordingViewError, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::recordingHeatmapLiveReceived,
            this, &IGridDataSource::recordingHeatmapLiveReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::recordingHeatmapHistoryReceived,
            this, &IGridDataSource::recordingHeatmapHistoryReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::recordingHeatmapHistoryError,
            this, &IGridDataSource::recordingHeatmapHistoryError, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleBarUpdateReceived,
            this, &RemoteGridDataSource::onCandleBarUpdateReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleBarClosedReceived,
            this, &RemoteGridDataSource::onCandleBarClosedReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::candleHistoryReceived,
            this, &RemoteGridDataSource::onCandleHistoryReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::serverConfigReceived,
            this, &RemoteGridDataSource::onServerConfigReceived, Qt::QueuedConnection);

    connect(&m_client, &SentinelStreamClient::connected,
            this,
            [this]{
                // Candle seq numbers restart with every server session.
                advanceCandleDeliveryGeneration();
                if (m_candleBuffer) m_candleBuffer->resetSequences();
                m_candleBackfill.requestRefresh();
                emit connectionStatusChanged(true);
                requestNextCandlePage();
            },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::disconnected,
            this,
            [this]{
                advanceCandleDeliveryGeneration();
                m_candleHistoryReady = false;
                m_candleBackfillTimer.stop();
                m_bookSnapshotTimer.stop();
                for (const auto& [symbol, pending] : m_pendingBookSnapshots) {
                    if (pending.stale) emit bookSnapshotStaleChanged(QString::fromStdString(symbol), false);
                }
                m_pendingBookSnapshots.clear();
                m_candleBackfill.disconnect();
                emit connectionStatusChanged(false);
            },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::subscriptionRefused,
            this, &IGridDataSource::subscriptionRefused, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::subscriptionAcknowledged,
            this, &IGridDataSource::subscriptionAcknowledged, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::errorOccurred,
            this, &IGridDataSource::errorOccurred, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::orderUpdated,
            this, &IGridDataSource::orderUpdated, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::algoOrderEventReceived,
            this, &RemoteGridDataSource::onAlgoOrderEventReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::pnlSnapshotReceived,
            this, &RemoteGridDataSource::onPnlSnapshotReceived, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::positionUpdated,
            this, &IGridDataSource::positionUpdated, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::riskOrderUpdated,
            this, &IGridDataSource::riskOrderUpdated, Qt::QueuedConnection);
}

namespace {
// --agent-host: no outbound request may name a symbol outside the allowlist (a subscribe makes the
// recorder subscribe upstream). Startup, the server's default symbol and every reconnect resubscribe
// all end here, so this is the one place that holds the line. Inactive: always true.
bool symbolPermitted(const std::string& symbol, const char* what) {
    const QString q = QString::fromStdString(symbol);
    if (AgentHostMode::symbolAllowed(q)) return true;
    sLog_Warning("agent-host: " << what << " refused: symbol=" << q);
    return false;
}
}  // namespace

void RemoteGridDataSource::connectToServer() {
    m_client.connectToServer();
}

void RemoteGridDataSource::subscribe(const QString& symbol) {
    if (!symbolPermitted(symbol.toStdString(), "subscribe")) return;
    const bool wasStale = isBookSnapshotStale(symbol);
    m_client.subscribe(symbol.toStdString());
    m_activeBookSymbols.insert(symbol.toStdString());
    m_pendingBookSnapshots[symbol.toStdString()] = {QDateTime::currentMSecsSinceEpoch() + 5000, false, false};
    if (!m_bookSnapshotTimer.isActive()) m_bookSnapshotTimer.start();
    if (wasStale) emit bookSnapshotStaleChanged(symbol, false);

    // Initialize replica on snapshot for authoritative range. A replica kept from an
    // earlier subscription stopped updating: cleared, so no book top derives from its
    // stale levels before the new snapshot (it seeded the chart ~$600 off, 2026-10-02).
    std::string s = symbol.toStdString();
    if (auto it = m_replicaBooks.find(s); it == m_replicaBooks.end()) {
        m_replicaBooks.emplace(s, std::make_unique<LiveOrderBook>(s));
    } else if (it->second) {
        it->second->clear();
    }
}


void RemoteGridDataSource::unsubscribe(const QString& symbol) {
    const bool wasStale = isBookSnapshotStale(symbol);
    m_client.unsubscribe(symbol.toStdString());
    m_activeBookSymbols.erase(symbol.toStdString());
    m_pendingBookSnapshots.erase(symbol.toStdString());
    if (m_pendingBookSnapshots.empty()) m_bookSnapshotTimer.stop();
    if (wasStale) emit bookSnapshotStaleChanged(symbol, false);
}

bool RemoteGridDataSource::isBookSnapshotStale(const QString& symbol) const {
    const auto it = m_pendingBookSnapshots.find(symbol.toStdString());
    return it != m_pendingBookSnapshots.end() && it->second.stale;
}

void RemoteGridDataSource::processBookSnapshotDeadlines(qint64 nowMs) {
    std::vector<std::string> retrySymbols;
    std::vector<QString> staleSymbols;
    for (auto& [symbol, pending] : m_pendingBookSnapshots) {
        if (pending.stale || nowMs < pending.deadlineMs) continue;
        if (!pending.retried) {
            if (!symbolPermitted(symbol, "book snapshot retry")) {
                pending.stale = true;
                pending.nextStaleRetryMs = nowMs + pending.staleRetryBackoffMs;
                staleSymbols.push_back(QString::fromStdString(symbol));
                continue;
            }
            pending.retried = true;
            pending.deadlineMs = nowMs + 5000;
            retrySymbols.push_back(symbol);
        } else {
            pending.stale = true;
            pending.nextStaleRetryMs = nowMs + pending.staleRetryBackoffMs;
            sLog_Error("Book snapshot stale: symbol=" << symbol << " retry=1");
            staleSymbols.push_back(QString::fromStdString(symbol));
        }
    }
    for (const auto& symbol : retrySymbols) {
        sLog_Warning("Book snapshot timeout: symbol=" << symbol << " retry=1");
        m_client.subscribe(symbol);
    }
    for (const auto& symbol : staleSymbols) {
        emit bookSnapshotStaleChanged(symbol, true);
        emit errorOccurred(QString("Order book snapshot stale: %1").arg(symbol));
    }
    if (std::all_of(m_pendingBookSnapshots.begin(), m_pendingBookSnapshots.end(),
        [](const auto& entry) { return entry.second.stale; })) m_bookSnapshotTimer.stop();
}

void RemoteGridDataSource::requestHeatmapHistory(const QString& symbol,
                                                 int64_t timeframeMs,
                                                 int64_t endTimeMs,
                                                 int count) {
    if (!symbolPermitted(symbol.toStdString(), "heatmap history")) return;
    m_client.requestHeatmapHistory(symbol.toStdString(), timeframeMs, endTimeMs, count);
}

void RemoteGridDataSource::registerRecordingView(const recording::LiveView& view) {
    if (!symbolPermitted(view.symbol, "recording view")) return;
    m_client.registerRecordingView(view);
}

void RemoteGridDataSource::releaseRecordingView(const recording::LiveView& view) {
    m_client.releaseRecordingView(view.symbol);
}

void RemoteGridDataSource::requestRecordingHeatmapHistory(
    const protocol::recordingwire::Request& request) {
    if (!symbolPermitted(request.symbol, "recording history")) return;
    m_client.requestRecordingHeatmapHistory(request);
}

void RemoteGridDataSource::requestFootprintHistory(const QString& symbol,
                                                   int64_t timeframeMs,
                                                   int64_t endTimeMs,
                                                   int count) {
    if (!symbolPermitted(symbol.toStdString(), "footprint history")) return;
    m_client.requestFootprintHistory(symbol.toStdString(), timeframeMs, endTimeMs, count);
}

void RemoteGridDataSource::setCandleHistoryViewport(const QString& symbol, int64_t timeframeSec,
                                                    qint64 startMs, qint64 endMs) {
    const bool selectionChanged = symbol != m_candleSymbol || timeframeSec != m_candleTimeframeSec;
    if (selectionChanged) advanceCandleDeliveryGeneration();
    const bool refresh = selectionChanged && !m_candleSymbol.isEmpty() &&
        m_candleBuffer->oldestTimeMs(symbol, timeframeSec) > 0;
    if (refresh)
        m_candleBuffer->resetSeriesForSelection(symbol, timeframeSec);
    m_candleSymbol = symbol;
    m_candleTimeframeSec = timeframeSec;
    m_candleHistoryReady = endMs > startMs && startMs > 0;
    const bool viewportChanged = m_candleBackfill.setViewport(symbol, timeframeSec, startMs, endMs);
    if (refresh) m_candleBackfill.requestRefresh();
    if (viewportChanged || refresh) {
        if (m_candleHistoryReady) requestNextCandlePage();
        else m_candleBackfillTimer.stop();
    }
}

void RemoteGridDataSource::advanceCandleDeliveryGeneration() {
    m_client.setCandleDeliveryGeneration(++m_candleDeliveryGeneration);
}

void RemoteGridDataSource::requestNextCandlePage() {
    if (!m_candleHistoryReady) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 oldest = m_candleBuffer->oldestTimeMs(m_candleSymbol, m_candleTimeframeSec);
    const bool full = m_candleBuffer->historyCapacityReached(m_candleSymbol, m_candleTimeframeSec);
    if (!m_candleBackfill.needsOlderData(oldest, full, now)) {
        m_candleBackfillTimer.stop();
        return;
    }
    if (const auto delay = m_candleBackfill.retryDelayMs(now); delay > 0) {
        // Keep the earliest trailing deadline while movement continues.
        if (!m_candleBackfillTimer.isActive() || m_candleBackfillTimer.remainingTime() > delay)
            m_candleBackfillTimer.start(static_cast<int>(delay));
        return;
    }
    const auto request = m_candleBackfill.next(oldest, full, now);
    if (!request) return;
    m_candleBackfillTimer.stop();
    if (!symbolPermitted(request->symbol.toStdString(), "candle history")) return;
    m_client.requestCandleHistory(request->symbol.toStdString(), request->timeframeSec,
                                 request->endSec, request->limit);
}

void RemoteGridDataSource::requestTpoHistory(const QString& symbol,
                                             int64_t timeframeMs,
                                             int sessionType,
                                             int64_t endTimeMs,
                                             int count,
                                             const QString& requestId) {
    if (!symbolPermitted(symbol.toStdString(), "tpo history")) return;
    m_client.requestTpoHistory(symbol.toStdString(), timeframeMs, sessionType, endTimeMs, count,
                               requestId.toStdString());
}

void RemoteGridDataSource::cancelTpoHistory(const QString& symbol, const QString& requestId) {
    m_client.cancelTpoHistory(symbol.toStdString(), requestId.toStdString());
}


void RemoteGridDataSource::sendTradeCommand(const trading::TradeCommand& command) {
    // --agent-host: the one place every TradeCommand passes (dock buttons, shortcuts, the chart's
    // TP/SL controls that /api/v1/input can reach). An agent-run GUI never trades.
    if (!AgentHostMode::tradingAllowed()) {
        sLog_Warning("agent-host: trade command dropped: action=" << static_cast<int>(command.action)
                     << " symbol=" << command.symbol);
        return;
    }
    m_client.sendTradeCommand(command);
}
const LiveOrderBook& RemoteGridDataSource::getDirectLiveOrderBook(const std::string& productId) const {
    auto it = m_replicaBooks.find(productId);
    if (it != m_replicaBooks.end() && it->second) {
        return *it->second;
    }
    static LiveOrderBook empty;
    return empty;
}

void RemoteGridDataSource::onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids,
                                               const std::vector<OrderBookLevel>& asks, quint64 deliveryGeneration) {
    std::string symbol = productId.toStdString();
    if (!m_activeBookSymbols.contains(symbol) ||
        deliveryGeneration != m_client.bookDeliveryGeneration(symbol)) return;
    // The stream server can send its still-empty local book immediately after
    // admission; only a two-sided upstream snapshot makes this replica ready.
    if (bids.empty() || asks.empty()) return;
    const bool wasStale = isBookSnapshotStale(productId);
    m_pendingBookSnapshots.erase(symbol);
    if (m_pendingBookSnapshots.empty()) m_bookSnapshotTimer.stop();

    // Create or reset replica
    if (m_replicaBooks.find(symbol) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(symbol, std::make_unique<LiveOrderBook>(symbol));
    }

    auto& book = *m_replicaBooks[symbol];

    // Re-initialize using banded range around best bid/ask.
    const double tickSize = m_serverConfig.orderbook.tickSize;
    const double bandPct = m_serverConfig.orderbook.bandPct;
    const auto [minPrice, maxPrice] = computeBandRange(bids, asks, bandPct);
    book.initialize(minPrice, maxPrice, tickSize);

    std::vector<BookLevelUpdate> updates;
    updates.reserve(bids.size() + asks.size());

    for (const auto& level : bids) {
        updates.push_back({true, level.price, level.size});
    }
    for (const auto& level : asks) {
        updates.push_back({false, level.price, level.size});
    }

    auto now = std::chrono::system_clock::now();
    std::vector<BookDelta> deltas;
    book.applyUpdates(updates, now, &deltas);
    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }

    sLog_Data("Book snapshot applied: symbol=" << productId
              << " bids=" << bids.size() << " asks=" << asks.size()
              << " band=[" << minPrice << ".." << maxPrice << "]"
              << " tick=" << tickSize << " deltas=" << deltas.size());
    if (wasStale) emit bookSnapshotStaleChanged(productId, false);
}

void RemoteGridDataSource::onServerConfigReceived(const ServerConfig& config) {
    m_serverConfig = config;
    GuiConfigStore::instance().setServerConfig(config);
}

void RemoteGridDataSource::onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates,
                                               quint64 deliveryGeneration) {
    onL2UpdateReceivedAt(productId, updates, deliveryGeneration, QDateTime::currentMSecsSinceEpoch());
}

void RemoteGridDataSource::onL2UpdateReceivedAt(const QString& productId,
                                                const std::vector<BookLevelUpdate>& updates,
                                                quint64 deliveryGeneration, qint64 nowMs) {
    std::string symbol = productId.toStdString();
    if (!m_activeBookSymbols.contains(symbol) ||
        deliveryGeneration != m_client.bookDeliveryGeneration(symbol)) return;
    if (auto pending = m_pendingBookSnapshots.find(symbol); pending != m_pendingBookSnapshots.end()) {
        if (pending->second.stale && nowMs >= pending->second.nextStaleRetryMs
            && symbolPermitted(symbol, "stale book snapshot retry")) {
            const qint64 delayMs = pending->second.staleRetryBackoffMs;
            pending->second.staleRetryBackoffMs = delayMs == 5000 ? 15000 : 60000;
            pending->second.nextStaleRetryMs = nowMs + pending->second.staleRetryBackoffMs;
            sLog_Warning("Stale book snapshot re-requested: symbol=" << productId
                         << " nextRetryMs=" << pending->second.staleRetryBackoffMs);
            m_client.subscribe(symbol);
        }
        return;
    }
    auto it = m_replicaBooks.find(symbol);
    if (it == m_replicaBooks.end()) {
        sLog_DataN(5000, "L2 update dropped, no replica book: symbol=" << productId
                   << " levels=" << updates.size());
        return;
    }

    auto& book = *it->second;

    thread_local std::vector<BookDelta> deltas;
    deltas.clear();

    auto now = std::chrono::system_clock::now();
    book.applyUpdates(updates, now, &deltas);

    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }
}

void RemoteGridDataSource::onHeatmapSliceReceived(const HeatmapSlice& slice) {
    emit heatmapSliceReceived(slice);
}

void RemoteGridDataSource::onFootprintSliceReceived(const FootprintSlice& slice) {
    emit footprintSliceReceived(slice);
}

void RemoteGridDataSource::onTpoSliceReceived(const TpoSlice& slice) {
    emit tpoSliceReceived(slice);
}

void RemoteGridDataSource::onVolumeProfileSliceReceived(const VolumeProfileSlice& slice) {
    emit volumeProfileSliceReceived(slice);
}

void RemoteGridDataSource::onHeatmapHistoryReceived(const QString& symbol,
                                                    int64_t timeframeMs,
                                                    int gridWidth,
                                                    int gridHeight,
                                                    int64_t requestEndMs,
                                                    int64_t oldestAvailableMs,
                                                    const QVector<SentinelStreamClient::HeatmapHistoryColumn>& columns) {
    QVector<HeatmapHistoryColumn> converted;
    converted.reserve(columns.size());
    for (const auto& col : columns) {
        HeatmapHistoryColumn out;
        out.bucketStartMs = col.bucketStartMs;
        out.bucketEndMs = col.bucketEndMs;
        out.minPrice = col.minPrice;
        out.maxPrice = col.maxPrice;
        out.tickSize = col.tickSize;
        out.intensity = col.intensity;
        out.liquidity = col.liquidity;
        out.liquidityScale = col.liquidityScale;
        converted.push_back(std::move(out));
    }
    emit heatmapHistoryReceived(symbol, timeframeMs, gridWidth, gridHeight,
                                requestEndMs, oldestAvailableMs, converted);
}

void RemoteGridDataSource::onCandleBarUpdateReceived(const QString& symbol,
                                                     int64_t timeframeSec,
                                                     int64_t,
                                                     int64_t seq,
                                                     const SentinelStreamClient::CandleBar& bar,
                                                     quint64 deliveryGeneration) {
    if (!m_candleBuffer || deliveryGeneration != m_candleDeliveryGeneration ||
        symbol != m_candleSymbol || timeframeSec != m_candleTimeframeSec) {
        return;
    }
    CandleSeriesBuffer::CandleBar out;
    out.timeStartMs = bar.timeStartMs;
    out.timeEndMs = bar.timeEndMs;
    out.open = bar.open;
    out.high = bar.high;
    out.low = bar.low;
    out.close = bar.close;
    out.volume = bar.volume;
    out.isClosed = bar.isClosed;
    out.seq = seq;
    m_candleBuffer->applyUpdate(symbol, timeframeSec, out, seq, false);
}

void RemoteGridDataSource::onCandleBarClosedReceived(const QString& symbol,
                                                     int64_t timeframeSec,
                                                     int64_t,
                                                     int64_t seq,
                                                     const SentinelStreamClient::CandleBar& bar,
                                                     quint64 deliveryGeneration) {
    if (!m_candleBuffer || deliveryGeneration != m_candleDeliveryGeneration ||
        symbol != m_candleSymbol || timeframeSec != m_candleTimeframeSec) {
        return;
    }
    CandleSeriesBuffer::CandleBar out;
    out.timeStartMs = bar.timeStartMs;
    out.timeEndMs = bar.timeEndMs;
    out.open = bar.open;
    out.high = bar.high;
    out.low = bar.low;
    out.close = bar.close;
    out.volume = bar.volume;
    out.isClosed = true;
    out.seq = seq;
    m_candleBuffer->applyUpdate(symbol, timeframeSec, out, seq, true);
}

void RemoteGridDataSource::onCandleHistoryReceived(const QString& symbol,
                                                   int64_t timeframeSec,
                                                   int64_t startTimeSec,
                                                   int64_t endTimeSec,
                                                   const QVector<SentinelStreamClient::CandleBar>& candles) {
    if (!m_candleBuffer) {
        return;
    }
    qint64 oldestReplyMs = 0;
    for (const auto& bar : candles) {
        if ((timeframeSec == 1 || bar.timeStartMs >= startTimeSec * 1000) &&
            bar.timeStartMs > 0 && bar.timeStartMs <= endTimeSec * 1000 &&
            (oldestReplyMs == 0 || bar.timeStartMs < oldestReplyMs)) oldestReplyMs = bar.timeStartMs;
    }
    const bool accepted = m_candleBackfill.accept(symbol, timeframeSec, startTimeSec, endTimeSec,
                                                 oldestReplyMs, QDateTime::currentMSecsSinceEpoch());
    if (!accepted) {
        sLog_Probe("candles.history", "stale reply symbol=" << symbol << " tfSec=" << timeframeSec);
        requestNextCandlePage(); // resume the latest selection after the stale flight
        return;
    }
    if (m_candleBackfill.scanPaused()) {
        sLog_Data("Candle history scan paused: symbol=" << symbol << " tfSec=" << timeframeSec
                  << " endSec=" << endTimeSec);
    }
    // History is merged by time and never touches the live seq stream.
    std::vector<CandleSeriesBuffer::CandleBar> bars;
    bars.reserve(static_cast<size_t>(candles.size()));
    for (const auto& bar : candles) {
        if (bar.timeStartMs <= 0 || bar.timeStartMs > endTimeSec * 1000 ||
            (timeframeSec != 1 && bar.timeStartMs < startTimeSec * 1000)) continue;
        CandleSeriesBuffer::CandleBar out;
        out.timeStartMs = bar.timeStartMs;
        out.timeEndMs = bar.timeEndMs;
        out.open = bar.open;
        out.high = bar.high;
        out.low = bar.low;
        out.close = bar.close;
        out.volume = bar.volume;
        out.isClosed = bar.isClosed;
        bars.push_back(out);
    }
    m_candleBuffer->applyHistory(symbol, timeframeSec, bars);
    sLog_Probe("candles.history",
               "applied symbol=" << symbol << " tfSec=" << timeframeSec
               << " t=[" << startTimeSec << ".." << endTimeSec << "]"
               << " count=" << candles.size());
    requestNextCandlePage(); // merge first so the next page sees the new oldest bar
}

void RemoteGridDataSource::sendAlgoCommand(const std::string& algoId,
                                            const std::string& action,
                                            const std::string& symbol,
                                            const trading::AlgoParams& params) {
    // --agent-host: the dock's Start/Stop buttons reach the server's algo runner; an agent-run GUI never does.
    if (!AgentHostMode::tradingAllowed()) {
        sLog_Warning("agent-host: algo command dropped: algo=" << algoId << " action=" << action << " symbol=" << symbol);
        return;
    }
    m_client.sendAlgoCommand(algoId, action, symbol, params);
}

void RemoteGridDataSource::onAlgoOrderEventReceived(const trading::AlgoOrderEvent& event) {
    emit algoOrderEventReceived(event);
}

void RemoteGridDataSource::onPnlSnapshotReceived(const trading::PnlSnapshot& snapshot) {
    emit pnlSnapshotReceived(snapshot);
}
