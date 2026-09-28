#include "TradeBlotterDock.hpp"

#include <QHeaderView>
#include <QTableWidget>

TradeBlotterDock::TradeBlotterDock(QWidget* parent)
    : QDockWidget(QStringLiteral("Trade Blotter"), parent) {
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("Side"), QStringLiteral("Qty"), QStringLiteral("Filled"), QStringLiteral("Status")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    // Row index cache is valid because this table is append-only and unsorted.
    setWidget(m_table);
}

void TradeBlotterDock::onOrderUpdated(const trading::OrderUpdate& update) {
    const QString orderIdStr = QString::fromStdString(update.orderId);
    int row = m_orderIdToRow.value(orderIdStr, -1);
    if (row < 0) {
        row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(orderIdStr));
        m_orderIdToRow.insert(orderIdStr, row);
        m_table->setItem(row, 1, new QTableWidgetItem(QString()));
        m_table->setItem(row, 2, new QTableWidgetItem(QString()));
        m_table->setItem(row, 3, new QTableWidgetItem(QString()));
        m_table->setItem(row, 4, new QTableWidgetItem(QString()));
    }
    m_table->item(row, 1)->setText(QString::fromUtf8(trading::toString(update.side)));
    m_table->item(row, 2)->setText(QString::number(update.qty, 'f', 4));
    m_table->item(row, 3)->setText(QString::number(update.filledQty, 'f', 4));
    m_table->item(row, 4)->setText(QString::fromUtf8(trading::toString(update.status)));
}
