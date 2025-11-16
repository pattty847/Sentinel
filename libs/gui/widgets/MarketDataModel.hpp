#pragma once

#include "DockablePanel.hpp"
#include <QAbstractTableModel>
#include <QTableView>
#include <QTimer>
#include <QHash>
#include <QString>
#include "../../core/marketdata/model/TradeData.h"

/**
 * Model for market data table.
 * Uses coalescing timer to batch updates for performance.
 */
class MarketDataModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit MarketDataModel(QObject* parent = nullptr);
    
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    
    void updateTrade(const Trade& trade);
    void clear();

private slots:
    void flushUpdates();

private:
    struct SymbolData {
        QString symbol;
        double price = 0.0;
        double change = 0.0;
        double changePercent = 0.0;
        int64_t lastUpdateTime = 0;
    };
    
    QHash<QString, SymbolData> m_symbolData;
    QStringList m_symbolOrder;  // Maintain insertion order
    QTimer* m_flushTimer;
    bool m_hasPendingUpdates = false;
};

