#include "RemoteGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <cmath>
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
                m_marketHealth.setTransport(MarketHealth::Transport::Connected);
                m_connectionActive = true;
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
                m_bookVersions.clear();
                m_candleBackfill.disconnect();
                m_marketHealth.setTransport(MarketHealth::Transport::Reconnecting);
                m_connectionActive = false;
                emit connectionStatusChanged(false);
            },
            Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::subscriptionRefused,
            this, [this](const QString& symbol, int cap, const QString& reason) {
                m_marketHealth.subscriptionRefused(symbol, reason);
                emit subscriptionRefused(symbol, cap, reason);
            }, Qt::QueuedConnection);
    connect(&m_client, &SentinelStreamClient::subscriptionAcknowledged,
            this, [this](const QString& symbol) {
                m_marketHealth.subscriptionAcknowledged(symbol);
                emit subscriptionAcknowledged(symbol);
            }, Qt::QueuedConnection);
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
    m_marketHealth.subscriptionRequested(symbol);
    m_client.subscribe(symbol.toStdString());
    m_activeBookSymbols.insert(symbol.toStdString());
    m_pendingBookSnapshots[symbol.toStdString()] = {QDateTime::currentMSecsSinceEpoch() + 5000, false, false};
    m_bookVersions.erase(symbol.toStdString());
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
    m_marketHealth.subscriptionReleased(symbol);
    m_client.unsubscribe(symbol.toStdString());
    m_activeBookSymbols.erase(symbol.toStdString());
    m_pendingBookSnapshots.erase(symbol.toStdString());
    m_bookVersions.erase(symbol.toStdString());
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
        m_marketHealth.bookStale(symbol);
        emit bookSnapshotStaleChanged(symbol, true);
        emit errorOccurred(QString("Order book snapshot stale: %1").arg(symbol));
    }
    if (std::all_of(m_pendingBookSnapshots.begin(), m_pendingBookSnapshots.end(),
        [](const auto& entry) { return entry.second.stale; })) m_bookSnapshotTimer.stop();
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
                                               const std::vector<OrderBookLevel>& asks, double tickSize,
                                               quint64 deliveryGeneration, const QString& status,
                                               uint64_t bookVersion) {
    std::string symbol = productId.toStdString();
    if (!m_activeBookSymbols.contains(symbol) ||
        deliveryGeneration != m_client.bookDeliveryGeneration(symbol)) return;
    if (auto previous = m_bookVersions.find(symbol); previous != m_bookVersions.end() &&
        (bookVersion == 0 || bookVersion < previous->second ||
         (bookVersion == previous->second && !m_pendingBookSnapshots.contains(symbol)))) return;
    // Any unavailable/one-sided server snapshot withdraws a previous ready book.
    if (bids.empty() || asks.empty() || !std::isfinite(tickSize) || tickSize <= 0 ||
        (!status.isEmpty() && status != QStringLiteral("ready"))) {
        if (auto it = m_replicaBooks.find(symbol); it != m_replicaBooks.end() && it->second)
            it->second->clear();
        m_marketHealth.bookUnavailable(productId, status);
        emit liveOrderBookUpdated(productId, {}); // Consumers withdraw cached rows/top.
        const auto pending = m_pendingBookSnapshots.try_emplace(
            symbol, PendingBookSnapshot{QDateTime::currentMSecsSinceEpoch() + 5000, false, false}).first;
        // Repeated unavailable replies must not restart the bounded retry clock.
        if (!pending->second.stale && !m_bookSnapshotTimer.isActive()) m_bookSnapshotTimer.start();
        if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;
        sLog_Warning("Replica snapshot unavailable: symbol=" << productId << " status=" << status
                     << " tick=" << tickSize);
        emit errorOccurred(QString("Order book unavailable: %1 (%2)").arg(productId, status));
        return;
    }
    const bool wasStale = isBookSnapshotStale(productId);

    // Create or reset replica
    if (m_replicaBooks.find(symbol) == m_replicaBooks.end()) {
        m_replicaBooks.emplace(symbol, std::make_unique<LiveOrderBook>(symbol));
    }

    auto& book = *m_replicaBooks[symbol];

    // Re-initialize using banded range around best bid/ask.
    const double bandPct = m_serverConfig.orderbook.bandPct;
    const auto [minPrice, maxPrice] = computeBandRange(bids, asks, bandPct);
    // All consumers share this replica. Preserve the server's resolution rather
    // than deriving a display tick that would merge its adjacent price levels.
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
    m_marketHealth.bookReceived(productId, QDateTime::currentMSecsSinceEpoch());
    if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;
    m_pendingBookSnapshots.erase(symbol);
    if (m_pendingBookSnapshots.empty()) m_bookSnapshotTimer.stop();
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
                                               double tickSize, quint64 deliveryGeneration,
                                               uint64_t bookVersion) {
    onL2UpdateReceivedAt(productId, updates, tickSize, deliveryGeneration,
                         QDateTime::currentMSecsSinceEpoch(), bookVersion);
}

void RemoteGridDataSource::onL2UpdateReceivedAt(const QString& productId,
                                                const std::vector<BookLevelUpdate>& updates,
                                                double tickSize, quint64 deliveryGeneration, qint64 nowMs,
                                                uint64_t bookVersion) {
    std::string symbol = productId.toStdString();
    if (!m_activeBookSymbols.contains(symbol) ||
        deliveryGeneration != m_client.bookDeliveryGeneration(symbol)) return;
    if (auto previous = m_bookVersions.find(symbol); previous != m_bookVersions.end() &&
        (bookVersion == 0 || bookVersion <= previous->second)) return;
    if (auto pending = m_pendingBookSnapshots.find(symbol); pending != m_pendingBookSnapshots.end()) {
        // A delta seen before a usable snapshot means any older queued snapshot
        // cannot restore a complete book.
        if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;
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
        if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;
        sLog_DataN(5000, "L2 update dropped, no replica book: symbol=" << productId
                   << " levels=" << updates.size());
        return;
    }

    auto& book = *it->second;
    if (!std::isfinite(tickSize) || tickSize <= 0.0 || book.getTickSize() != tickSize) {
        sLog_Warning("L2 update tick disagrees with snapshot: symbol=" << productId
                     << " snapshotTick=" << book.getTickSize() << " deltaTick=" << tickSize);
        book.clear();
        m_marketHealth.bookUnavailable(productId, QStringLiteral("Book tick changed; waiting for snapshot"));
        emit liveOrderBookUpdated(productId, {});
        if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;
        const auto pending = m_pendingBookSnapshots.try_emplace(
            symbol, PendingBookSnapshot{nowMs + 5000, false, false}).first;
        if (!pending->second.stale && !m_bookSnapshotTimer.isActive()) m_bookSnapshotTimer.start();
        emit errorOccurred(QString("Order book tick mismatch: %1").arg(productId));
        return;
    }

    thread_local std::vector<BookDelta> deltas;
    deltas.clear();

    auto now = std::chrono::system_clock::now();
    book.applyUpdates(updates, now, &deltas);
    m_marketHealth.bookReceived(productId, nowMs, false);
    if (bookVersion > 0) m_bookVersions[symbol] = bookVersion;

    if (!deltas.empty()) {
        emit liveOrderBookUpdated(productId, deltas);
    }
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
