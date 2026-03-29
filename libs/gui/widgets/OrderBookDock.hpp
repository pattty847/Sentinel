#ifndef ORDERBOOKDOCK_HPP
#define ORDERBOOKDOCK_HPP

#include "DockablePanel.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QTableWidget>
#include <QStyledItemDelegate>
#include <vector>
#include <unordered_map>

struct BookDelta;

class DomBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum BarSide { BarBid, BarAsk };
    explicit DomBarDelegate(BarSide side, QObject* parent = nullptr);
    void setMaxQty(double maxQty) { m_maxQty = maxQty; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    BarSide m_side;
    double m_maxQty = 1.0;
};

class OrderBookDock : public DockablePanel {
    Q_OBJECT

public:
    explicit OrderBookDock(QWidget* parent = nullptr);
    ~OrderBookDock() override = default;

    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

private slots:
    void onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas);
    void onTradeReceived(const Trade& trade);

private:
    void connectToMarketData();
    void updateSpreadDisplay(double bidPrice, double bidSize, double askPrice, double askSize);
    void setupSpreadLayout();
    void refreshDomTable();
    double priceToTick(double price) const;
    int priceToRow(double price) const;

    static constexpr int kDomLevelsPerSide = 12;
    static constexpr int kTradeCountCap = 1000;

    // UI
    QFrame* m_spreadFrame = nullptr;
    QLabel* m_symbolLabel = nullptr;
    QLabel* m_bidPriceLabel = nullptr;
    QLabel* m_bidSizeLabel = nullptr;
    QFrame* m_bidFrame = nullptr;
    QLabel* m_askPriceLabel = nullptr;
    QLabel* m_askSizeLabel = nullptr;
    QFrame* m_askFrame = nullptr;
    QLabel* m_spreadLabel = nullptr;
    QLabel* m_midLabel = nullptr;
    QTableWidget* m_domTable = nullptr;
    DomBarDelegate* m_bidBarDelegate = nullptr;
    DomBarDelegate* m_askBarDelegate = nullptr;

    QString m_currentSymbol;
    double m_lastBidPrice = 0.0;
    double m_lastBidSize = 0.0;
    double m_lastAskPrice = 0.0;
    double m_lastAskSize = 0.0;
    double m_tickSize = 0.0;
    double m_minPrice = 0.0;

    // Per-price trade counts for BUYS/SELLS/DELTA (price key = tick-rounded)
    struct PriceCounts {
        int buys = 0;
        int sells = 0;
        int delta() const { return buys - sells; }
    };
    std::unordered_map<double, PriceCounts> m_tradeCountsByPrice;
    std::vector<std::pair<double, bool>> m_tradeHistory;
};

#endif // ORDERBOOKDOCK_HPP
