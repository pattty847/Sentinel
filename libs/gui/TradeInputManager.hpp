#pragma once

#include <QObject>
#include <QString>

#include "datasources/IGridDataSource.hpp"

class QWidget;

class TradeInputManager : public QObject {
    Q_OBJECT
public:
    explicit TradeInputManager(IGridDataSource* dataSource, QWidget* parentWidget, QObject* parent = nullptr);

    void setSymbol(const QString& symbol);

private:
    void sendMarketOrder(trading::OrderSide side);
    void sendAction(trading::TradeAction action);

    IGridDataSource* m_dataSource = nullptr;
    QWidget* m_parentWidget = nullptr;
    QString m_symbol = QStringLiteral("BTC-USD");
};
