#pragma once

#include <QObject>
#include <QString>
#include <functional>

#include "datasources/IGridDataSource.hpp"
#include "../core/trading/TradingTypes.hpp"

class QWidget;

class TradeInputManager : public QObject {
    Q_OBJECT
public:
    explicit TradeInputManager(IGridDataSource* dataSource,
                               QWidget* parentWidget,
                               std::function<double()> quantityProvider,
                               double fallbackQty,
                               QObject* parent = nullptr);

    void setSymbol(const QString& symbol);

private:
    void sendMarketOrder(trading::OrderSide side);
    void sendAction(trading::TradeAction action);

    IGridDataSource* m_dataSource = nullptr;
    QWidget* m_parentWidget = nullptr;
    std::function<double()> m_quantityProvider;
    double m_fallbackQty = 1.0;
    QString m_symbol = QStringLiteral("BTC-USD");
};
