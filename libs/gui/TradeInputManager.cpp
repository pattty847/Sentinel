#include "TradeInputManager.hpp"

#include <QDateTime>
#include <QShortcut>
#include <QUuid>
#include <QWidget>

TradeInputManager::TradeInputManager(IGridDataSource* dataSource,
                                   QWidget* parentWidget,
                                   std::function<double()> quantityProvider,
                                   double fallbackQty,
                                   QObject* parent)
    : QObject(parent)
    , m_dataSource(dataSource)
    , m_parentWidget(parentWidget)
    , m_quantityProvider(std::move(quantityProvider))
    , m_fallbackQty(fallbackQty > 0.0 ? fallbackQty : 1.0) {
    auto* buy = new QShortcut(QKeySequence(Qt::Key_B), parentWidget);
    auto* sell = new QShortcut(QKeySequence(Qt::Key_S), parentWidget);
    auto* flatten = new QShortcut(QKeySequence(Qt::Key_F), parentWidget);
    auto* cancelAll = new QShortcut(QKeySequence(Qt::Key_C), parentWidget);

    connect(buy, &QShortcut::activated, this, [this]() { sendMarketOrder(trading::OrderSide::Buy); });
    connect(sell, &QShortcut::activated, this, [this]() { sendMarketOrder(trading::OrderSide::Sell); });
    connect(flatten, &QShortcut::activated, this, [this]() { sendAction(trading::TradeAction::Flatten); });
    connect(cancelAll, &QShortcut::activated, this, [this]() { sendAction(trading::TradeAction::CancelAll); });
}

void TradeInputManager::setSymbol(const QString& symbol) {
    if (!symbol.isEmpty()) {
        m_symbol = symbol;
    }
}

void TradeInputManager::sendMarketOrder(trading::OrderSide side) {
    if (!m_dataSource) {
        return;
    }
    trading::TradeCommand cmd;
    cmd.commandId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = m_symbol.toStdString();
    cmd.side = side;
    cmd.orderType = trading::OrderType::Market;
    double qty = m_fallbackQty;
    if (m_quantityProvider) {
        const double candidate = m_quantityProvider();
        if (candidate > 0.0) {
            qty = candidate;
        }
    }
    cmd.qty = qty;
    cmd.timestamp = QDateTime::currentMSecsSinceEpoch();
    m_dataSource->sendTradeCommand(cmd);
}

void TradeInputManager::sendAction(trading::TradeAction action) {
    if (!m_dataSource) {
        return;
    }
    trading::TradeCommand cmd;
    cmd.commandId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    cmd.action = action;
    cmd.symbol = m_symbol.toStdString();
    cmd.orderType = trading::OrderType::Market;
    cmd.timestamp = QDateTime::currentMSecsSinceEpoch();
    m_dataSource->sendTradeCommand(cmd);
}
