#pragma once

#include "DockablePanel.hpp"
#include "MarketDataModel.hpp"
#include <QTableView>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QSortFilterProxyModel>

/**
 * Market data panel showing symbols, prices, and changes.
 * Updates from MarketDataCore trade signals with coalescing for performance.
 */
class MarketDataPanel : public DockablePanel {
    Q_OBJECT

public:
    explicit MarketDataPanel(QWidget* parent = nullptr);
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

private slots:
    void onTradeReceived(const Trade& trade);
    void onSymbolClicked(const QModelIndex& index);

private:
    QTableView* m_tableView;
    MarketDataModel* m_model;
    QSortFilterProxyModel* m_proxyModel;
    QLineEdit* m_filterEdit;
};

