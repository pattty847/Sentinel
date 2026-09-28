#pragma once

#include <QDockWidget>
#include <QHash>

#include "../../core/trading/TradingTypes.hpp"

class QTableWidget;

class TradeBlotterDock : public QDockWidget {
    Q_OBJECT
public:
    explicit TradeBlotterDock(QWidget* parent = nullptr);

public slots:
    void onOrderUpdated(const trading::OrderUpdate& update);

private:
    QTableWidget* m_table = nullptr;
    QHash<QString, int> m_orderIdToRow;
};
