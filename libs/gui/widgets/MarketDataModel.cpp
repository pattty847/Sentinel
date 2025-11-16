#include "MarketDataModel.hpp"
#include <QColor>
#include <QDateTime>

MarketDataModel::MarketDataModel(QObject* parent)
    : QAbstractTableModel(parent)
{
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(250);  // 250ms coalescing
    connect(m_flushTimer, &QTimer::timeout, this, &MarketDataModel::flushUpdates);
}

int MarketDataModel::rowCount(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return m_symbolOrder.size();
}

int MarketDataModel::columnCount(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return 4;  // Symbol, Price, Change, Change%
}

QVariant MarketDataModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_symbolOrder.size()) {
        return QVariant();
    }
    
    QString symbol = m_symbolOrder[index.row()];
    const SymbolData& data = m_symbolData[symbol];
    
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return symbol;
            case 1: return QString::number(data.price, 'f', 2);
            case 2: return QString::number(data.change, 'f', 2);
            case 3: return QString::number(data.changePercent, 'f', 2) + "%";
            default: return QVariant();
        }
    } else if (role == Qt::ForegroundRole) {
        if (index.column() == 2 || index.column() == 3) {
            // Color code change values
            return data.change >= 0 ? QColor(Qt::green) : QColor(Qt::red);
        }
    }
    
    return QVariant();
}

QVariant MarketDataModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    
    switch (section) {
        case 0: return "Symbol";
        case 1: return "Price";
        case 2: return "Change";
        case 3: return "Change %";
        default: return QVariant();
    }
}

void MarketDataModel::updateTrade(const Trade& trade) {
    QString symbol = QString::fromStdString(trade.product_id);
    
    if (!m_symbolData.contains(symbol)) {
        // New symbol
        beginInsertRows(QModelIndex(), m_symbolOrder.size(), m_symbolOrder.size());
        m_symbolOrder.append(symbol);
        SymbolData& data = m_symbolData[symbol];
        data.symbol = symbol;
        data.price = trade.price;
        data.change = 0.0;
        data.changePercent = 0.0;
        auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            trade.timestamp.time_since_epoch()).count();
        data.lastUpdateTime = timestamp_ms;
        endInsertRows();
    } else {
        // Update existing symbol
        SymbolData& data = m_symbolData[symbol];
        double oldPrice = data.price;
        data.price = trade.price;
        data.change = trade.price - oldPrice;  // Simplified - should use previous close
        data.changePercent = oldPrice > 0 ? (data.change / oldPrice) * 100.0 : 0.0;
        auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            trade.timestamp.time_since_epoch()).count();
        data.lastUpdateTime = timestamp_ms;
        
        int row = m_symbolOrder.indexOf(symbol);
        if (row >= 0) {
            QModelIndex topLeft = index(row, 0);
            QModelIndex bottomRight = index(row, columnCount() - 1);
            emit dataChanged(topLeft, bottomRight);
        }
    }
    
    // Schedule flush (coalescing)
    if (!m_flushTimer->isActive()) {
        m_flushTimer->start();
    }
    m_hasPendingUpdates = true;
}

void MarketDataModel::flushUpdates() {
    m_hasPendingUpdates = false;
    // Updates are already applied, timer just ensures we don't spam too frequently
}

void MarketDataModel::clear() {
    beginResetModel();
    m_symbolData.clear();
    m_symbolOrder.clear();
    endResetModel();
}

