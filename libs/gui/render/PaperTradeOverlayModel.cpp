#include "PaperTradeOverlayModel.hpp"
#include "SentinelLogging.hpp"

#include <cmath>
#include <algorithm>

PaperTradeOverlayModel::PaperTradeOverlayModel(QObject* parent)
    : QObject(parent) {}

double PaperTradeOverlayModel::priceFromScreenY(double screenY) const {
    if (!m_mappingProvider) {
        return 0.0;
    }
    const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
    if (!mapping.valid) {
        return 0.0;
    }
    return mapping.screenYToPrice(screenY);
}

bool PaperTradeOverlayModel::isLongPosition() const {
    return m_hasPosition && m_position.positionQty > 0.0;
}

void PaperTradeOverlayModel::emitRiskStateChanged() {
    const bool nowVisible = riskConfirmVisible();
    emit riskStateChanged();
    if (nowVisible != m_lastRiskConfirmVisible) {
        m_lastRiskConfirmVisible = nowVisible;
        emit riskConfirmVisibleChanged();
    }
}

QString PaperTradeOverlayModel::mappingProbeText() const {
    if (!m_mappingProvider) {
        return QStringLiteral("mapping=none");
    }
    const MappingFrameContext frame = m_mappingProvider->currentFrameContext();
    const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
    return QString("viewValid=%1 dragging=%2 view=[%3..%4] viewPrice=[%5..%6] pan=(%7,%8)"
                   " mapValid=%9 mapPrice=[%10..%11] drawY=%12 drawH=%13 srcY=%14 srcH=%15")
        .arg(frame.viewportValid ? 1 : 0)
        .arg(frame.viewportDragging ? 1 : 0)
        .arg(frame.viewportTimeStart)
        .arg(frame.viewportTimeEnd)
        .arg(frame.viewportMinPrice)
        .arg(frame.viewportMaxPrice)
        .arg(frame.viewportPanVisualOffset.x())
        .arg(frame.viewportPanVisualOffset.y())
        .arg(mapping.valid ? 1 : 0)
        .arg(mapping.viewMinPrice)
        .arg(mapping.viewMaxPrice)
        .arg(mapping.drawRect.y())
        .arg(mapping.drawRect.height())
        .arg(mapping.srcRect.y())
        .arg(mapping.srcRect.height());
}

void PaperTradeOverlayModel::setSymbol(const QString& symbol) {
    if (m_symbol == symbol) {
        return;
    }
    m_symbol = symbol;
    m_orders.clear();
    m_position = trading::PositionUpdate{};
    m_hasPosition = false;
    m_lastTradePrice = 0.0;
    m_hasLastTrade = false;
    m_activeRisk = RiskState{};
    m_stagedRisk = RiskState{};
    m_hasStagedRisk = false;
    m_draggingLeg.clear();
    emit symbolChanged();
    emit openOrdersChanged();
    emit activePositionChanged();
    emitRiskStateChanged();
}

void PaperTradeOverlayModel::setMappingProvider(QObject* provider) {
    if (m_mappingProviderObject == provider) {
        return;
    }
    m_mappingProviderObject = provider;
    m_mappingProvider = qobject_cast<ITimeAxisMappingProvider*>(provider);
    emit mappingProviderChanged();
}

QVariantList PaperTradeOverlayModel::openOrders() const {
    std::vector<const ManualOrderState*> sortedOrders;
    sortedOrders.reserve(m_orders.size());
    for (const auto& [orderId, order] : m_orders) {
        Q_UNUSED(orderId);
        sortedOrders.push_back(&order);
    }
    std::sort(sortedOrders.begin(), sortedOrders.end(), [](const ManualOrderState* lhs, const ManualOrderState* rhs) {
        if (lhs->side != rhs->side) {
            return static_cast<int>(lhs->side) < static_cast<int>(rhs->side);
        }
        if (lhs->side == trading::OrderSide::Buy) {
            return lhs->price > rhs->price;
        }
        return lhs->price < rhs->price;
    });

    QVariantList out;
    out.reserve(static_cast<int>(sortedOrders.size()));
    for (const ManualOrderState* order : sortedOrders) {
        QVariantMap item;
        item["orderId"] = QString::fromStdString(order->orderId);
        item["side"] = QString::fromUtf8(trading::toString(order->side));
        item["qty"] = order->qty;
        item["filledQty"] = order->filledQty;
        item["price"] = order->price;
        item["status"] = QString::fromUtf8(trading::toString(order->status));
        out.push_back(item);
    }
    return out;
}

QVariantMap PaperTradeOverlayModel::activePosition() const {
    QVariantMap out;
    if (!m_hasPosition || std::abs(m_position.positionQty) < 1e-12) {
        return out;
    }

    const double qty = m_position.positionQty;
    const double totalPnl = m_position.unrealizedPnl + m_position.realizedPnl;
    const double notional = std::abs(qty) * m_position.avgPrice;
    const double pnlPct = (notional > 0.0) ? (totalPnl / notional) * 100.0 : 0.0;
    const double openPnlPct = (notional > 0.0) ? (m_position.unrealizedPnl / notional) * 100.0 : 0.0;
    double markPrice = m_position.avgPrice;
    if (std::abs(qty) > 1e-12) {
        if (qty > 0.0) {
            markPrice = m_position.avgPrice + (m_position.unrealizedPnl / qty);
        } else {
            markPrice = m_position.avgPrice - (m_position.unrealizedPnl / std::abs(qty));
        }
    }
    if (m_hasLastTrade && m_lastTradePrice > 0.0) {
        markPrice = m_lastTradePrice;
    }

    out["side"] = qty >= 0.0 ? "LONG" : "SHORT";
    out["qty"] = qty;
    out["absQty"] = std::abs(qty);
    out["entryPrice"] = m_position.avgPrice;
    out["markPrice"] = markPrice;
    out["lastPrice"] = m_hasLastTrade ? m_lastTradePrice : markPrice;
    out["openPnl"] = m_position.unrealizedPnl;
    out["openPnlPct"] = openPnlPct;
    out["unrealizedPnl"] = m_position.unrealizedPnl;
    out["realizedPnl"] = m_position.realizedPnl;
    out["totalPnl"] = totalPnl;
    out["pnlPct"] = pnlPct;
    return out;
}

QVariantMap PaperTradeOverlayModel::riskState() const {
    QVariantMap out;
    const RiskState& live = m_hasStagedRisk ? m_stagedRisk : m_activeRisk;
    out["hasActiveTakeProfit"] = m_activeRisk.hasTakeProfit;
    out["activeTakeProfitPrice"] = m_activeRisk.takeProfitPrice;
    out["hasActiveStopLoss"] = m_activeRisk.hasStopLoss;
    out["activeStopLossPrice"] = m_activeRisk.stopLossPrice;
    out["hasTakeProfit"] = live.hasTakeProfit;
    out["takeProfitPrice"] = live.takeProfitPrice;
    out["hasStopLoss"] = live.hasStopLoss;
    out["stopLossPrice"] = live.stopLossPrice;
    out["hasStagedRiskChanges"] = m_hasStagedRisk;
    out["draggingLeg"] = m_draggingLeg;
    return out;
}

void PaperTradeOverlayModel::onTradeReceived(const Trade& trade) {
    if (!m_symbol.isEmpty() && QString::fromStdString(trade.product_id) != m_symbol) {
        return;
    }
    if (trade.price <= 0.0) {
        return;
    }
    m_lastTradePrice = trade.price;
    m_hasLastTrade = true;
    sLog_Probe("papertrade.trade", "symbol=" << trade.product_id
               << " price=" << trade.price << " size=" << trade.size
               << " side=" << (trade.side == AggressorSide::Buy ? "BUY"
                               : trade.side == AggressorSide::Sell ? "SELL" : "UNKNOWN")
               << " hasPosition=" << (m_hasPosition && std::abs(m_position.positionQty) > 1e-12)
               << " positionQty=" << m_position.positionQty
               << " entry=" << m_position.avgPrice
               << " uPnl=" << m_position.unrealizedPnl
               << " " << mappingProbeText());
    if (m_hasPosition && std::abs(m_position.positionQty) > 1e-12) {
        emit activePositionChanged();
    }
}

void PaperTradeOverlayModel::onOrderUpdated(const trading::OrderUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }
    if (!update.algoId.empty()) {
        return;
    }

    const bool isActive = update.status == trading::OrderStatus::Open ||
                          update.status == trading::OrderStatus::New ||
                          update.status == trading::OrderStatus::Partial;
    if (!isActive) {
        if (m_orders.erase(update.orderId) > 0) {
            emit openOrdersChanged();
        }
        return;
    }

    ManualOrderState state;
    state.orderId = update.orderId;
    state.side = update.side;
    state.qty = update.qty;
    state.filledQty = update.filledQty;
    state.price = (update.limitPrice > 0.0) ? update.limitPrice : update.avgPrice;
    state.status = update.status;
    m_orders[update.orderId] = state;
    sLog_Probe("papertrade.order", "symbol=" << update.symbol
               << " orderId=" << update.orderId
               << " side=" << trading::toString(update.side)
               << " qty=" << update.qty << " filled=" << update.filledQty
               << " price=" << state.price
               << " status=" << trading::toString(update.status)
               << " " << mappingProbeText());
    emit openOrdersChanged();
}

void PaperTradeOverlayModel::onPositionUpdated(const trading::PositionUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }
    m_position = update;
    m_hasPosition = true;
    const double totalPnl = update.unrealizedPnl + update.realizedPnl;
    sLog_Probe("papertrade.position", "symbol=" << update.symbol
               << " qty=" << update.positionQty << " entry=" << update.avgPrice
               << " lastTrade=" << m_lastTradePrice
               << " uPnl=" << update.unrealizedPnl << " rPnl=" << update.realizedPnl
               << " totalPnl=" << totalPnl
               << " " << mappingProbeText());
    if (std::abs(update.positionQty) < 1e-12) {
        m_activeRisk = RiskState{};
        m_stagedRisk = RiskState{};
        m_hasStagedRisk = false;
        m_draggingLeg.clear();
        emitRiskStateChanged();
    }
    emit activePositionChanged();
}

void PaperTradeOverlayModel::onRiskOrderUpdated(const trading::RiskOrderUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }
    m_activeRisk.hasTakeProfit = update.hasTakeProfit;
    m_activeRisk.takeProfitPrice = update.takeProfitPrice;
    m_activeRisk.hasStopLoss = update.hasStopLoss;
    m_activeRisk.stopLossPrice = update.stopLossPrice;
    if (!m_hasStagedRisk) {
        m_stagedRisk = m_activeRisk;
    }
    emitRiskStateChanged();
}

bool PaperTradeOverlayModel::canShowRiskControls() const {
    return m_hasPosition && std::abs(m_position.positionQty) > 1e-12;
}

bool PaperTradeOverlayModel::hasStagedRiskChanges() const {
    return m_hasStagedRisk;
}

bool PaperTradeOverlayModel::beginRiskDrag(const QString& leg) {
    if (!canShowRiskControls()) {
        return false;
    }
    if (leg != QStringLiteral("tp") && leg != QStringLiteral("sl")) {
        return false;
    }
    m_stagedRisk = m_activeRisk;
    m_hasStagedRisk = true;
    m_draggingLeg = leg;
    emitRiskStateChanged();
    return true;
}

void PaperTradeOverlayModel::updateRiskDrag(double screenY) {
    if (!m_hasStagedRisk || m_draggingLeg.isEmpty() || !canShowRiskControls()) {
        return;
    }
    const double price = priceFromScreenY(screenY);
    if (price <= 0.0) {
        return;
    }
    const bool isLong = isLongPosition();
    if (m_draggingLeg == QStringLiteral("tp")) {
        if ((isLong && price <= m_position.avgPrice) || (!isLong && price >= m_position.avgPrice)) {
            return;
        }
        m_stagedRisk.hasTakeProfit = true;
        m_stagedRisk.takeProfitPrice = price;
    } else if (m_draggingLeg == QStringLiteral("sl")) {
        if ((isLong && price >= m_position.avgPrice) || (!isLong && price <= m_position.avgPrice)) {
            return;
        }
        m_stagedRisk.hasStopLoss = true;
        m_stagedRisk.stopLossPrice = price;
    }
    emitRiskStateChanged();
}

void PaperTradeOverlayModel::endRiskDrag() {
    if (m_draggingLeg.isEmpty()) {
        return;
    }
    m_draggingLeg.clear();
    emitRiskStateChanged();
}

void PaperTradeOverlayModel::discardStagedRisk() {
    m_stagedRisk = m_activeRisk;
    m_hasStagedRisk = false;
    m_draggingLeg.clear();
    emitRiskStateChanged();
}

void PaperTradeOverlayModel::setRiskState(bool hasTakeProfit,
                                          double takeProfitPrice,
                                          bool hasStopLoss,
                                          double stopLossPrice) {
    m_stagedRisk.hasTakeProfit = hasTakeProfit;
    m_stagedRisk.takeProfitPrice = takeProfitPrice;
    m_stagedRisk.hasStopLoss = hasStopLoss;
    m_stagedRisk.stopLossPrice = stopLossPrice;
    m_hasStagedRisk = true;
    emitRiskStateChanged();
}

void PaperTradeOverlayModel::confirmStagedRisk() {
    if (!m_hasStagedRisk || !canShowRiskControls()) {
        return;
    }
    const RiskState confirmed = m_stagedRisk;
    m_activeRisk = confirmed;
    m_hasStagedRisk = false;
    m_draggingLeg.clear();
    emitRiskStateChanged();
    emit applyAttachedRiskRequested(confirmed.hasTakeProfit,
                                    confirmed.takeProfitPrice,
                                    confirmed.hasStopLoss,
                                    confirmed.stopLossPrice);
}

void PaperTradeOverlayModel::logPositionOverlaySample(double displayedEntryY,
                                                      double displayedMarkY,
                                                      const QString& source) {
    static const bool kProbe = sentinel::logging::probeEnabled("papertrade.overlay");
    if (!kProbe) {
        return;
    }
    if (!m_hasPosition || std::abs(m_position.positionQty) < 1e-12) {
        return;
    }
    double markPrice = m_position.avgPrice;
    if (m_hasLastTrade && m_lastTradePrice > 0.0) {
        markPrice = m_lastTradePrice;
    } else if (std::abs(m_position.positionQty) > 1e-12) {
        markPrice = m_position.positionQty > 0.0
            ? m_position.avgPrice + (m_position.unrealizedPnl / m_position.positionQty)
            : m_position.avgPrice - (m_position.unrealizedPnl / std::abs(m_position.positionQty));
    }

    double authoritativeEntryY = std::numeric_limits<double>::quiet_NaN();
    double authoritativeMarkY = std::numeric_limits<double>::quiet_NaN();
    if (m_mappingProvider) {
        const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
        if (mapping.valid) {
            authoritativeEntryY = mapping.priceToScreenY(m_position.avgPrice);
            authoritativeMarkY = mapping.priceToScreenY(markPrice);
        }
    }

    sLog_Probe("papertrade.overlay", "position source=" << source
               << " entry=" << m_position.avgPrice << " mark=" << markPrice
               << " shownEntryY=" << displayedEntryY << " shownMarkY=" << displayedMarkY
               << " mappedEntryY=" << authoritativeEntryY << " mappedMarkY=" << authoritativeMarkY
               << " entryDeltaY=" << (displayedEntryY - authoritativeEntryY)
               << " markDeltaY=" << (displayedMarkY - authoritativeMarkY)
               << " " << mappingProbeText());
}

void PaperTradeOverlayModel::logOrderOverlaySample(const QString& orderId,
                                                   double displayedY,
                                                   const QString& source) {
    static const bool kProbe = sentinel::logging::probeEnabled("papertrade.overlay");
    if (!kProbe) {
        return;
    }
    const auto it = m_orders.find(orderId.toStdString());
    if (it == m_orders.end()) {
        return;
    }
    double authoritativeY = std::numeric_limits<double>::quiet_NaN();
    if (m_mappingProvider) {
        const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
        if (mapping.valid) {
            authoritativeY = mapping.priceToScreenY(it->second.price);
        }
    }

    sLog_Probe("papertrade.overlay", "order source=" << source
               << " orderId=" << orderId << " price=" << it->second.price
               << " side=" << trading::toString(it->second.side)
               << " shownY=" << displayedY << " mappedY=" << authoritativeY
               << " deltaY=" << (displayedY - authoritativeY)
               << " " << mappingProbeText());
}
