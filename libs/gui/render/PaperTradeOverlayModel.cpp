#include "PaperTradeOverlayModel.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <cmath>
#include <algorithm>

namespace {
bool paperTradeDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("SENTINEL_PAPERTRADE_DEBUG");
    return enabled;
}

QString jsonString(const QString& value) {
    QString out = value;
    out.replace("\\", "\\\\");
    out.replace("\"", "\\\"");
    out.replace("\n", "\\n");
    return QString("\"%1\"").arg(out);
}

QString jsonNumber(double value) {
    if (!std::isfinite(value)) {
        return QStringLiteral("null");
    }
    return QString::number(value, 'f', 6);
}

QString jsonInteger(qint64 value) {
    return QString::number(value);
}
}

PaperTradeOverlayModel::PaperTradeOverlayModel(QObject* parent)
    : QObject(parent) {}

qint64 PaperTradeOverlayModel::nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

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

bool PaperTradeOverlayModel::shouldLogOverlaySample(qint64& lastLogMs) const {
    if (!paperTradeDebugEnabled()) {
        return false;
    }
    const qint64 current = nowMs();
    if (current - lastLogMs < 80) {
        return false;
    }
    lastLogMs = current;
    return true;
}

void PaperTradeOverlayModel::appendDebugLog(const char* hypothesisId,
                                            const char* message,
                                            const QString& dataJson) const {
    if (!paperTradeDebugEnabled()) {
        return;
    }

    const QString path = QDir::current().absoluteFilePath(".cursor/debug.log");
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QString runId = qEnvironmentVariableIsSet("SENTINEL_PAPERTRADE_DEBUG_RUN")
        ? qEnvironmentVariable("SENTINEL_PAPERTRADE_DEBUG_RUN")
        : QStringLiteral("default-run");

    QTextStream stream(&out);
    stream << "{\"sessionId\":\"papertrade-debug\""
           << ",\"runId\":" << jsonString(runId)
           << ",\"hypothesisId\":" << jsonString(QString::fromUtf8(hypothesisId))
           << ",\"location\":\"PaperTradeOverlayModel\""
           << ",\"message\":" << jsonString(QString::fromUtf8(message))
           << ",\"data\":" << dataJson
           << ",\"timestamp\":" << nowMs()
           << "}\n";
}

QString PaperTradeOverlayModel::currentViewportJson() const {
    if (!m_mappingProvider) {
        return QStringLiteral("null");
    }
    const MappingFrameContext frame = m_mappingProvider->currentFrameContext();
    return QString("{\"valid\":%1,\"dragging\":%2,\"timeStart\":%3,\"timeEnd\":%4,"
                   "\"minPrice\":%5,\"maxPrice\":%6,\"panX\":%7,\"panY\":%8}")
        .arg(frame.viewportValid ? "true" : "false")
        .arg(frame.viewportDragging ? "true" : "false")
        .arg(jsonInteger(frame.viewportTimeStart))
        .arg(jsonInteger(frame.viewportTimeEnd))
        .arg(jsonNumber(frame.viewportMinPrice))
        .arg(jsonNumber(frame.viewportMaxPrice))
        .arg(jsonNumber(frame.viewportPanVisualOffset.x()))
        .arg(jsonNumber(frame.viewportPanVisualOffset.y()));
}

QString PaperTradeOverlayModel::currentMappingJson() const {
    if (!m_mappingProvider) {
        return QStringLiteral("null");
    }
    const TimeAxisMapping mapping = m_mappingProvider->currentTimeAxisMapping();
    return QString("{\"valid\":%1,\"viewMinPrice\":%2,\"viewMaxPrice\":%3,"
                   "\"drawY\":%4,\"drawH\":%5,\"srcY\":%6,\"srcH\":%7}")
        .arg(mapping.valid ? "true" : "false")
        .arg(jsonNumber(mapping.viewMinPrice))
        .arg(jsonNumber(mapping.viewMaxPrice))
        .arg(jsonNumber(mapping.drawRect.y()))
        .arg(jsonNumber(mapping.drawRect.height()))
        .arg(jsonNumber(mapping.srcRect.y()))
        .arg(jsonNumber(mapping.srcRect.height()));
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
    appendDebugLog("PT1", "trade_received",
                   QString("{\"symbol\":%1,\"tradePrice\":%2,\"tradeSize\":%3,\"side\":%4,"
                           "\"hasPosition\":%5,\"positionQty\":%6,\"entryPrice\":%7,"
                           "\"uPnl\":%8,\"viewport\":%9,\"mapping\":%10}")
                       .arg(jsonString(QString::fromStdString(trade.product_id)))
                       .arg(jsonNumber(trade.price))
                       .arg(jsonNumber(trade.size))
                       .arg(jsonString(trade.side == AggressorSide::Buy ? "BUY"
                                       : trade.side == AggressorSide::Sell ? "SELL"
                                                                          : "UNKNOWN"))
                       .arg((m_hasPosition && std::abs(m_position.positionQty) > 1e-12) ? "true" : "false")
                       .arg(jsonNumber(m_position.positionQty))
                       .arg(jsonNumber(m_position.avgPrice))
                       .arg(jsonNumber(m_position.unrealizedPnl))
                       .arg(currentViewportJson())
                       .arg(currentMappingJson()));
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
    appendDebugLog("PT1", "order_update",
                   QString("{\"symbol\":%1,\"orderId\":%2,\"side\":%3,\"qty\":%4,"
                           "\"filledQty\":%5,\"price\":%6,\"status\":%7,\"viewport\":%8,\"mapping\":%9}")
                       .arg(jsonString(QString::fromStdString(update.symbol)))
                       .arg(jsonString(QString::fromStdString(update.orderId)))
                       .arg(jsonString(QString::fromUtf8(trading::toString(update.side))))
                       .arg(jsonNumber(update.qty))
                       .arg(jsonNumber(update.filledQty))
                       .arg(jsonNumber(state.price))
                       .arg(jsonString(QString::fromUtf8(trading::toString(update.status))))
                       .arg(currentViewportJson())
                       .arg(currentMappingJson()));
    emit openOrdersChanged();
}

void PaperTradeOverlayModel::onPositionUpdated(const trading::PositionUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }
    m_position = update;
    m_hasPosition = true;
    const double totalPnl = update.unrealizedPnl + update.realizedPnl;
    appendDebugLog("PT1", "position_update",
                   QString("{\"symbol\":%1,\"positionQty\":%2,\"entryPrice\":%3,"
                           "\"lastTradePrice\":%4,\"uPnl\":%5,\"rPnl\":%6,\"totalPnl\":%7,"
                           "\"viewport\":%8,\"mapping\":%9}")
                       .arg(jsonString(QString::fromStdString(update.symbol)))
                       .arg(jsonNumber(update.positionQty))
                       .arg(jsonNumber(update.avgPrice))
                       .arg(jsonNumber(m_lastTradePrice))
                       .arg(jsonNumber(update.unrealizedPnl))
                       .arg(jsonNumber(update.realizedPnl))
                       .arg(jsonNumber(totalPnl))
                       .arg(currentViewportJson())
                       .arg(currentMappingJson()));
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
    if (!m_hasPosition || std::abs(m_position.positionQty) < 1e-12) {
        return;
    }
    if (!shouldLogOverlaySample(m_lastPositionOverlayLogMs)) {
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

    appendDebugLog("PT2", "position_overlay_sample",
                   QString("{\"source\":%1,\"entryPrice\":%2,\"markPrice\":%3,"
                           "\"displayedEntryY\":%4,\"displayedMarkY\":%5,"
                           "\"authoritativeEntryY\":%6,\"authoritativeMarkY\":%7,"
                           "\"entryDeltaY\":%8,\"markDeltaY\":%9,"
                           "\"viewport\":%10,\"mapping\":%11}")
                       .arg(jsonString(source))
                       .arg(jsonNumber(m_position.avgPrice))
                       .arg(jsonNumber(markPrice))
                       .arg(jsonNumber(displayedEntryY))
                       .arg(jsonNumber(displayedMarkY))
                       .arg(jsonNumber(authoritativeEntryY))
                       .arg(jsonNumber(authoritativeMarkY))
                       .arg(jsonNumber(displayedEntryY - authoritativeEntryY))
                       .arg(jsonNumber(displayedMarkY - authoritativeMarkY))
                       .arg(currentViewportJson())
                       .arg(currentMappingJson()));
}

void PaperTradeOverlayModel::logOrderOverlaySample(const QString& orderId,
                                                   double displayedY,
                                                   const QString& source) {
    if (!shouldLogOverlaySample(m_lastOrderOverlayLogMs)) {
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

    appendDebugLog("PT3", "order_overlay_sample",
                   QString("{\"source\":%1,\"orderId\":%2,\"price\":%3,\"side\":%4,"
                           "\"displayedY\":%5,\"authoritativeY\":%6,\"deltaY\":%7,"
                           "\"viewport\":%8,\"mapping\":%9}")
                       .arg(jsonString(source))
                       .arg(jsonString(orderId))
                       .arg(jsonNumber(it->second.price))
                       .arg(jsonString(QString::fromUtf8(trading::toString(it->second.side))))
                       .arg(jsonNumber(displayedY))
                       .arg(jsonNumber(authoritativeY))
                       .arg(jsonNumber(displayedY - authoritativeY))
                       .arg(currentViewportJson())
                       .arg(currentMappingJson()));
}
